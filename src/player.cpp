#include "player.h"

#include <AudioFileSourceBuffer.h>
#ifdef PLAYER_SPIRAM
#include <AudioFileSourceSPIRAMBuffer.h>
#else
#include <AudioFileSourceBuffer.h>
#endif

#ifdef PLAYER_DEBUG
// Called when a metadata event occurs (i.e. an ID3 tag, an ICY block, etc.
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string)
{
    const char *ptr = reinterpret_cast<const char *>(cbData);
    (void)isUnicode; // Punt this ball for now
    // Note that the type and string may be in PROGMEM, so copy them to RAM for printf
    char s1[32], s2[64];
    strncpy_P(s1, type, sizeof(s1));
    s1[sizeof(s1) - 1] = 0;
    strncpy_P(s2, string, sizeof(s2));
    s2[sizeof(s2) - 1] = 0;
    Serial.printf("METADATA(%s) '%s' = '%s'\n", ptr, s1, s2);
    Serial.flush();
}

// Called when there's a warning or error (like a buffer underflow or decode hiccup)
void StatusCallback(void *cbData, int code, const char *string)
{
    const char *ptr = reinterpret_cast<const char *>(cbData);
    // Note that the string may be in PROGMEM, so copy it to RAM for printf
    char s1[64];
    strncpy_P(s1, string, sizeof(s1));
    s1[sizeof(s1) - 1] = 0;
    Serial.printf("STATUS(%s) '%d' = '%s'\n", ptr, code, s1);
    Serial.flush();
}
#endif

Player::Player() :
    _volume(PLAYER_INIT_VOLUME),
    _actions(nullptr),
    _previousAction(NONE),
    _playlistUrls(),
    _playlistIndex(-1),
    _started(0),
    _bufferDirty(0),
    _idleSince(1),
    _trackOffset(0)
{
    _file = new AudioFileSourceHTTPStream();
#ifdef PLAYER_SPIRAM
    _buffer = new AudioFileSourceSPIRAMBuffer(_file, PLAYER_SPIRAM_CS, PLAYER_BUFFER_SIZE);
#else
    _buffer = new AudioFileSourceBuffer(_file, PLAYER_BUFFER_SIZE);
#endif
    _output = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S, 32, AudioOutputI2S::APLL_ENABLE);
    _mp3 = new AudioGeneratorMP3a();

#ifdef PLAYER_DEBUG
    _file->RegisterMetadataCB(MDCallback, (void *)"HTTP");
    _buffer->RegisterStatusCB(StatusCallback, (void *)"buffer");
    _mp3->RegisterStatusCB(StatusCallback, (void *)"mp3");
#endif

    _output->SetRate(44100);
    _output->SetOutputModeMono(true);

    _beeper = new BeepGenerator(_output);
}

Player::~Player()
{
    stop(true);
    delete _mp3;
    delete _output;
    delete _buffer;
    delete _file;
    delete _beeper;
}

// Load playlist (M3U format) from URL
void Player::playlist(const char* url) {
    _clearPlaylist();

    HTTPClient http;
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        int len = http.getSize();

        uint8_t buff[256] = { 0 };
        WiFiClient* stream = http.getStreamPtr();

        ulong start = millis();

        while(http.connected() && (len > 0 || len == -1)) {
            size_t size = stream->available();
            if (size) {
                int c = stream->readBytesUntil('\n', buff, std::min((size_t)sizeof(buff), size)) + 1;
                if (c > 0 && buff[0] != '#') {
                    char* playlistEntry = (char *)malloc(c);
                    strncpy(playlistEntry, (char *)buff, c-1);
                    playlistEntry[c-1] = '\0';
                    if (playlistEntry[c-2] == '\r') playlistEntry[c-2] = '\0';
                    _playlistUrls.push_back(playlistEntry);
                    Serial.printf("Loaded playlist entry: %s\n", playlistEntry);
                }
                if (len > 0) {
                    len -= c;
                }
            }

            if (millis() - start > PLAYER_HTTP_TIMEOUT) {
                Serial.println("ERROR Timeout in GET request");
                _clearPlaylist();
                break;
            }
            delay(1);
        }
    } else {
        Serial.printf("ERROR GET failed, response code was: %d\n", httpCode);
        beep(500, PLAYER_FREQ_ERROR);
        _addAction(SILENCE, 500, nullptr, true);
        beep(1500, PLAYER_FREQ_ERROR);
    }
    http.end();

    next(false);
}

// Start playing url
void Player::start(const char *url)
{
    _started = millis();
    if (_trackOffset != 0) {
        _file->open(url, _trackOffset);
        _trackOffset = 0;
    } else {
        _file->open(url);
    }
    _buffer->seek(0, SEEK_SET);
    Serial.printf("Starting audio stream from %s\n", url);
    _mp3->begin(_buffer, _output);
    if (_bufferDirty == 0) {
        _setVolume();
    } else {
        _output->SetGainF2P6(0);
    }
}

// Stop playing
// clearPlaylist - remove all tracks from playlist
// stopPause - clause pause state after expiry
void Player::stop(bool clearPlaylist, bool stopPause)
{
    _output->SetGainF2P6(0);

    _clearActions();

#ifdef PLAYER_SPIRAM
    _bufferDirty = 16;
#endif
    _addAction(stopPause ? PAUSE_STOP : STOP, 0, nullptr, true);
    _removeActions(PAUSE);
    if (stopPause) {
        _trackOffset = _file->getPos();
        _trackOffset = (_trackOffset > PLAYER_BUFFER_SIZE) ? _trackOffset - PLAYER_BUFFER_SIZE : 0;
        Serial.printf("PAUSE -> STOP at file pos. %d\n", _trackOffset);
    } else {
        _removeActions(PAUSE_STOP);
    }

    if (clearPlaylist)
        _clearPlaylist();
}

// Play next track
// beep - play short confirmation sound
void Player::next(bool playBeep) {
    if (isPlaying()) stop(false);
    _playlistIndex++;
    if (playBeep) {
        beep(20, PLAYER_FREQ_BEEP);
    }
    _addAction(PLAYLIST, 0, nullptr);
    _removeActions(PAUSE);
}

// Play previous track
// beep - play short confirmation sound
void Player::previous(bool playBeep) {
    if (isPlaying()) stop(false);
    
    if ((millis() - _started) < PLAYER_PREV_RESTART)
        _playlistIndex--;
    if (playBeep) {
        beep(20, PLAYER_FREQ_BEEP);
    }
    _addAction(PLAYLIST, 0, nullptr);
    _removeActions(PAUSE);
}

// Pause / unpause
void Player::pause() {
    bool wasPaused = _removeActions(PAUSE);
    bool wasPauseStopped = _removeActions(PAUSE_STOP);

    if (wasPauseStopped) {
        _addAction(PLAYLIST, 0, nullptr);
    } else if (wasPaused) {
        _setVolume();
        beep(20, PLAYER_FREQ_BEEP);
    } else {
        beep(20, PLAYER_FREQ_BEEP);
        _addAction(PAUSE, millis(), nullptr);
    }
}

// Increase volume up to PLAYER_MAX_VOLUME
void Player::volumeUp() {
    if (_volume < PLAYER_MAX_VOLUME) _volume++;
    _setVolume();
    beep(20, PLAYER_FREQ_BEEP);

}

// Decrease volume
void Player::volumeDown() {
    if (_volume > 0) _volume--;
    _setVolume();
    beep(20, PLAYER_FREQ_BEEP);
}

// Returns true if playback in progress (or paused)
bool Player::isPlaying()
{
    return _mp3->isRunning();
}

// Plays a short beep sound
void Player::beep(ulong ms, uint16_t freq) {
    uint16_t *freq_param = (uint16_t*)malloc(sizeof(uint16_t));
    (*freq_param) = freq;
    _addAction(SILENCE, 50, nullptr, true);
    _addAction(BEEP, ms, freq_param, true);
}

// How long have we been idle?
// returns 0 if not idle or duration in ms
ulong Player::idleSince() {
    if (_idleSince == 0) return 0;
    else return millis() - _idleSince;
}

// Starts a task on core 0 the handles the worker loop for player
bool Player::begin()
{
    disableCore0WDT();
    xTaskCreatePinnedToCore(
        Player::_worker,
        "AudioPlayer",
        10000,
        this,
        10,
        &this->_playerTask,
        0);
    return true;
}

// Callback from player task
void Player::_worker(void *args)
{
    for (;;)
    {
        reinterpret_cast<Player*>(args)->_loop();
    }
}

// Worker loop
void Player::_loop()
{
    bool idle = false;
    _nextAction();
    switch (_currentAction.code) {
        case PLAYLIST: {
            if (_playlistIndex >= 0 && _playlistIndex < _playlistUrls.size()) {
                start(_playlistUrls[_playlistIndex]);
            } else {
#ifdef PLAYER_DEBUG
                Serial.println("Playlist is finished, stopping here");
#endif
                _addAction(SILENCE, 100, nullptr);
                _playlistIndex = -1;
            }
        } break;
        case SILENCE: {
            if (_previousAction != SILENCE) _output->SetGainF2P6(0);
            _loopSilence();
        } break;
        case BEEP: {
            _setVolume();
            _beeper->loop(*reinterpret_cast<uint16_t*>(_currentAction.param));
        } break;
        case PAUSE: {
            if (_previousAction != PAUSE) {
                _output->SetGainF2P6(0);
                if (_mp3->isRunning()) _mp3->loop();
            }
            delay(100);
        } break;
        case PAUSE_STOP: {
            if (_previousAction != PAUSE_STOP) {
                _mp3->stop();
            }
            idle = true;
            delay(100);
        } break;
        case STOP: {
            _mp3->stop();
        } break;
        case NONE: {
            if (_previousAction == PAUSE || _previousAction == PAUSE_STOP || _previousAction == SILENCE) {
                _setVolume();
            }
            if (_mp3->isRunning()) {
                if (_bufferDirty > 0) {
                    _bufferDirty--;
                    if (_bufferDirty == 0) _setVolume();
                }
                if (!_mp3->loop()) {
                    _mp3->stop();
                    next(false);
                }
            } else {
                idle = true;
                delay(100);
            }
        } break;
    }
    if (idle) {
        if (_idleSince == 0) _idleSince = millis();
    } else {
        _idleSince = 0;
    }
    _previousAction = _currentAction.code;
}

// Clear actions stack and free memory
// keep - specify action code that should stay on stack
// return true if keep was found on action stack
bool Player::_clearActions(ActionCode keep) {
    bool found = false;
    Action* action = _actions;
    _actions = nullptr;
    while (action) {
        Action* next = action->next;
        if (action->code == keep && _actions == nullptr) {
            action->next = nullptr;
            _actions = action;
            found = true;
        } else {
            free(action);
            action = next;
        }
    }
    return found;
}

// Add a new action
// code - action code
// timestamp - code specify, may indicate action start or end times
// param - code specific
// asNext - insert on bottom (i.e. next action) of stack vs. on top
void Player::_addAction(ActionCode code, ulong timestamp, void* param, bool asNext) {
    Action* newAction = (Action*)malloc(sizeof(Action));
    newAction->code = code;
    newAction->timestamp = timestamp;
    newAction->param = param;
    newAction->next = nullptr;

    if (code == BEEP || code == SILENCE)
        newAction->timestamp += millis();

    if (asNext) {
        newAction->next = _actions;
        _actions = newAction;
    } else {
        if (_actions == nullptr) {
            _actions = newAction;
        } else {
            Action* action = _actions;
            while (action) {
                if (action->next == nullptr) {
                    action->next = newAction;
                    break;
                }
                action = action->next;
            }
        }
    }
#ifdef PLAYER_DEBUG
    _dumpActions();
#endif
}

// Removes all actions of a specific type from the stack
// code - action code specifying the type
// return true if the specified action was found at least once
bool Player::_removeActions(ActionCode code) {
    bool found = false;
    Action *newActions = nullptr;
    Action *action = _actions;
    Action *prev = nullptr, *next = nullptr;

    while (action) {
        next = action->next;
        if (action->code == code) {
            if (prev) prev->next = nullptr;
            free(action);
            found = true;
        } else {
            if (prev) prev->next = action;
            prev = action;
            if (!newActions) newActions = action;
        }
        action = next;
    }
    _actions = newActions;

    return found;
}

// Gives the next action from (the bottom of) the stack
// Handles param related actions and removes the action from the stack if appropriate
// Note that this is the last location where we have a reference to the current action's param
// so we free up the memory here if appropriate
// Returns the next action code
void Player::_nextAction() {
    if (_actions == nullptr) {
        if (_currentAction.param != nullptr) free(_currentAction.param);

        _currentAction.code = NONE;
        _currentAction.timestamp = 0;
        _currentAction.param = nullptr;
        _currentAction.next = nullptr;
        return;
    }

    if (_currentAction.param != nullptr && _currentAction.param != _actions->param) free(_currentAction.param);

    _currentAction.code = _actions->code;
    _currentAction.timestamp = _actions->timestamp;
    _currentAction.param = _actions->param;
    _currentAction.next = _actions->next;
#ifdef PLAYER_DEBUG    
    if (_currentAction.code != _previousAction) _dumpActions();
#endif
    bool consume = true;
    switch (_currentAction.code) {
        case SILENCE:
        case BEEP: {
            consume = (_currentAction.timestamp < millis());
        } break;
        case PAUSE: {
            consume = (millis() - _currentAction.timestamp > PLAYER_PAUSE_EXPIRY);
        } break;
        case PAUSE_STOP: {
            consume = false;
        } break;
        default: { }
    }
    if (consume) {
#ifdef PLAYER_DEBUG
        Serial.printf("Consumed action %d from stack\n", (int)_currentAction.code);
#endif
        Player::Action* action = _actions;
        _actions = action->next;
        free(action);
        if (_currentAction.code == PAUSE) {
            stop(false, true);
        }
    }
}

// Convenience function, dump actions to serial
void Player::_dumpActions() {
    Serial.printf("Action Stack:");
    Player::Action* action = _actions;

    const char * names[] = {
        "NONE",
        "PAUSE",
        "PLAYLIST",
        "STOP",
        "BEEP",
        "SILENCE",
        "PAUSE_STOP"
    };

    while (action) {
        Serial.printf("   %d:%s ( %p, %lu, %p )", (int)action->code, names[(int)action->code], action, action->timestamp, action->param);
        action = action->next;
    }
    Serial.printf("\n");
    Serial.flush();
}

// Adjust output volume
void Player::_setVolume() {
#ifdef PLAYER_DEBUG
    Serial.print("_setVolume() => "); Serial.println(_volume);
#endif
    uint8_t gain = _volume * PLAYER_VOLUME_FACTOR;
    _output->SetGainF2P6(gain);
}

// Fills i2s buffer with zero samples
void Player::_loopSilence() {
    ulong end = millis() + 50;
    int16_t sample[2] = { 0, 0 };
    while (millis() < end) {
        _output->ConsumeSample(sample);
    }
}

// Clear playlist and free memory
void Player::_clearPlaylist(void) {
    stop(false);
    for (int i=0; i<_playlistUrls.size(); i++) {
        free(_playlistUrls[i]);
    }
    _playlistUrls.clear();
    _playlistIndex = -1;
}
