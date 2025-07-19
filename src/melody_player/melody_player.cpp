#include "melody_player.h"

/**
 * https://stackoverflow.com/questions/24609271/errormake-unique-is-not-a-member-of-std
 */
template<typename T, typename... Args> std::unique_ptr<T> make_unique(Args&&... args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

void MelodyPlayer::play() {
  if (melodyState == nullptr) { return; }

  turnOn();
  state = State::PLAY;

  melodyState->advance();
  while (melodyState->getIndex() + melodyState->isSilence() < melodyState->melody.getLength()) {
    NoteDuration computedNote = melodyState->getCurrentComputedNote();
    if (debug)
      Serial.println(String("Playing: frequency:") + computedNote.frequency
                     + " duration:" + computedNote.duration);
    if (melodyState->isSilence()) {
      ledcWriteTone(pwmChannel, 0);
      delay(0.3f * computedNote.duration);
    } else {
      ledcWriteTone(pwmChannel, computedNote.frequency);
      delay(computedNote.duration);
    }
    melodyState->advance();
  }
  stop();
}

void MelodyPlayer::play(Melody& melody) {
  if (!melody) { return; }
  melodyState = make_unique<MelodyState>(melody);
  play();
}

void changeTone(MelodyPlayer* player) {
  // The last silence is not reproduced
  player->melodyState->advance();
  if (player->melodyState->getIndex() + player->melodyState->isSilence()
      < player->melodyState->melody.getLength()) {
    NoteDuration computedNote(player->melodyState->getCurrentComputedNote());

    float duration = player->melodyState->getRemainingNoteDuration();
    if (duration > 0) {
      player->melodyState->resetRemainingNoteDuration();
    } else {
      if (player->melodyState->isSilence()) {
        duration = 0.3f * computedNote.duration;
      } else {
        duration = computedNote.duration;
      }
    }
    if (player->debug)
      Serial.println(String("Playing async: freq=") + computedNote.frequency + " dur=" + duration
                     + " iteration=" + player->melodyState->getIndex());

    if (player->melodyState->isSilence()) {
      if(!player->muted)
      {
        ledcWriteTone(player->pwmChannel, 0);
      }

      player->ticker.once_ms(duration, changeTone, player);
    } else {

      if(!player->muted)
      {
        ledcWriteTone(player->pwmChannel, computedNote.frequency);
        ledcWrite(player->pwmChannel, player->volume);
      }

      player->ticker.once_ms(duration, changeTone, player);
    }
    player->supportSemiNote = millis() + duration;
  } else {
    // End of the melody
    player->stop();
    if(player->loop)
    { // Loop mode => start over
      player->playAsync();
    }
    else if(player->stopCallback != NULL)
    {
      player->stopCallback();
    }
  }
}

void MelodyPlayer::playAsync() {
  if (melodyState == nullptr) { return; }

  turnOn();
  state = State::PLAY;

  // Start immediately
  ticker.once(0, changeTone, this);
}

void MelodyPlayer::playAsync(Melody& melody, bool loopMelody, void(*callback)(void)) {
  if (!melody) { return; }
  melodyState = make_unique<MelodyState>(melody);
  loop = loopMelody;
  stopCallback = callback;
  playAsync();
}

void MelodyPlayer::stop() {
  if (melodyState == nullptr) { return; }

  haltPlay();
  state = State::STOP;
  melodyState->reset();
}

void MelodyPlayer::pause() {
  if (melodyState == nullptr) { return; }

  haltPlay();
  state = State::PAUSE;
  melodyState->saveRemainingNoteDuration(supportSemiNote);
}

void MelodyPlayer::transferMelodyTo(MelodyPlayer& destPlayer) {
  if (melodyState == nullptr) { return; }

  destPlayer.stop();

  bool playing = isPlaying();

  haltPlay();
  state = State::STOP;
  melodyState->saveRemainingNoteDuration(supportSemiNote);
  destPlayer.melodyState = std::move(melodyState);

  if (playing) {
    destPlayer.playAsync();
  } else {
    destPlayer.state = state;
  }
}

void MelodyPlayer::duplicateMelodyTo(MelodyPlayer& destPlayer) {
  if (melodyState == nullptr) { return; }

  destPlayer.stop();
  destPlayer.melodyState = make_unique<MelodyState>(*(this->melodyState));
  destPlayer.melodyState->saveRemainingNoteDuration(supportSemiNote);

  if (isPlaying()) {
    destPlayer.playAsync();
  } else {
    destPlayer.state = state;
  }
}

MelodyPlayer::MelodyPlayer(unsigned char pin, unsigned char pwmChannel, bool offLevel)
  : pin(pin), pwmChannel(pwmChannel), offLevel(offLevel), state(State::STOP), melodyState(nullptr) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, offLevel);
};

void MelodyPlayer::haltPlay() {
  // Stop player, but do not reset the melodyState
  ticker.detach();
  turnOff();
}

void MelodyPlayer::turnOn() {

  const int resolution = 8;
  // 2000 is a frequency, it will be changed at the first play
  ledcSetup(pwmChannel, 2000, resolution);
  ledcAttachPin(pin, pwmChannel);
  ledcWrite(pwmChannel, volume);
}

void MelodyPlayer::setVolume(byte newVolume) {
  volume = newVolume/2;
  if(state == State::PLAY)
  {
    ledcWrite(pwmChannel, volume);
  }
}

void MelodyPlayer::turnOff() {
  ledcWrite(pwmChannel, 0);
  ledcDetachPin(pin);

  pinMode(pin, OUTPUT);
  digitalWrite(pin, offLevel);
}

void MelodyPlayer::mute() {
  muted = true;
}

void MelodyPlayer::unmute() {
  ledcAttachPin(pin, pwmChannel);
  muted = false;
}

void MelodyPlayer::changeTempo(int newTempo) {
  if (melodyState == nullptr) { return; }
  melodyState->changeTempo(newTempo);
}