#ifndef MELODY_PLAYER_H
#define MELODY_PLAYER_H

#include "melody.h"
#include <Ticker.h>
#include <memory>

class MelodyPlayer {
public:
#ifdef ESP32
  /**
   * pwmChannel is optional and you have to configure it only if you play will
   * simultaneous melodies.
   * volume is the PWM duty cycle (0-255), default is 125.
   */
  MelodyPlayer(unsigned char pin, unsigned char pwmChannel = 0, bool offLevel = HIGH, unsigned char volume = 125);
#else
  MelodyPlayer(unsigned char pin, bool offLevel = HIGH);
#endif

  /**
   * Play the last melody in a synchrounus (blocking) way.
   * If the melody is not valid, this call has no effect.
   */
  void play();

  /**
   * Play the given melody in a synchronous (blocking) way.
   * If the melody is not valid, this call has no effect.
   */
  void play(Melody& melody);

  /**
   * Play the last melody in asynchronous way (return immediately).
   * If the melody is not valid, this call has no effect.
   */
  void playAsync();

  /**
   * Play the given melody in asynchronous way (return immediately).
   * If the melody is not valid, this call has no effect.
   */
  void playAsync(Melody& melody);

  /**
   * Stop the current melody.
   * Then, if you will call play() or playAsync(), the melody restarts from the begin.
   */
  void stop();

  /**
   * Pause the current melody.
   * Then, if you will call play() or playAsync(), the melody continues from
   * where it was paused.
   */
  void pause();

  /**
   * Tell if playing.
   */
  bool isPlaying() const {
    return state == State::PLAY;
  }

  /**
   * Move the current melody and player's state to the given destination Player.
   * The source player stops and lose the reference to the actual melody (i.e. you have to call
   * play(Melody) to make the source player play again).
   */
  void transferMelodyTo(MelodyPlayer& destination);

  /**
   * Duplicate the current melody and player's state to the given destination Player.
   * Both players remains indipendent from each other (e.g. the melody can be independently
   * stopped/paused/played).
   */
  void duplicateMelodyTo(MelodyPlayer& destination);

  /**
   * Set the volume (PWM duty cycle) for the buzzer.
   * @param volume PWM duty cycle value (0-255). 0 = silent, 255 = maximum volume.
   */
  void setVolume(unsigned char volume);

  /**
   * Get the current volume (PWM duty cycle) setting.
   * @return Current volume value (0-255).
   */
  unsigned char getVolume() const;

private:
  unsigned char pin;

#ifdef ESP32
  unsigned char pwmChannel;
  unsigned char volume;
#endif

  /**
   * The voltage to turn off the buzzer.
   *
   * NOTE: Passive buzzers have 2 states: the "rest" state (no power consumption) and the "active"
   * state (high power consumption). To emit sound, it have to oscillate between these 2 states. If
   * it stops in the active state, it doesn't emit sound, but it continues to consume energy,
   * heating the buzzer and possibly damaging itself.
   */
  bool offLevel;

  /**
   * Store the playback state of a melody and provide the methods to control it.
   */
  class MelodyState {
  public:
    MelodyState() : first(true), index(0), remainingNoteTime(0){};
    MelodyState(const Melody& melody)
      : melody(melody), first(true), silence(false), index(0), remainingNoteTime(0){};
    Melody melody;

    unsigned short getIndex() const {
      return index;
    }

    bool isSilence() const {
      return silence;
    }

    /**
     * Advance the melody index by one step. If there is a pending partial note it hasn't any
     * effect.
     */
    void advance() {
      if (first) {
        first = false;
        return;
      }
      if (remainingNoteTime != 0) { return; }

      if (melody.getAutomaticSilence()) {
        if (silence) {
          index++;
          silence = false;
        } else {
          silence = true;
        }
      } else {
        index++;
      }
    }

    /**
     * Reset the state of the melody (i.e. a melody just loaded).
     */
    void reset() {
      first = true;
      index = 0;
      remainingNoteTime = 0;
      silence = false;
    }

    /**
     * Save the time to finish the current note.
     */
    void saveRemainingNoteDuration(unsigned long supportSemiNote) {
      remainingNoteTime = supportSemiNote - millis();
      // Ignore partial reproduction if the current value is below the threshold. This is needed
      // since Ticker may struggle with tight timings.
      if (remainingNoteTime < 10) { remainingNoteTime = 0; }
    }

    /**
     * Clear the partial note duration. It should be called after reproduction of the partial note
     * and it is propedeutic to advance() method.
     */
    void resetRemainingNoteDuration() {
      remainingNoteTime = 0;
    }

    /**
     * Get the remaining duration of the latest "saved" note.
     */
    unsigned short getRemainingNoteDuration() const {
      return remainingNoteTime;
    }

    /**
     * Get the current note. The duration is absolute and expressed in milliseconds.
     */
    NoteDuration getCurrentComputedNote() const {
      NoteDuration note = melody.getNote(getIndex());
      note.duration = melody.getTimeUnit() * note.duration;
      // because the fixed point notation
      note.duration /= 2;
      return note;
    }

  private:
    bool first;
    bool silence;
    unsigned short index;

    /**
     * Variable to support precise pauses and move/duplicate melodies between Players.
     * Value are expressed in milliseconds.
     */
    unsigned short remainingNoteTime;
  };

  enum class State { STOP, PLAY, PAUSE };

  State state;

  std::unique_ptr<MelodyState> melodyState;

  unsigned long supportSemiNote;

  Ticker ticker;

  const static bool debug = false;

  /**
   * Change the current note with the next one.
   */
  friend void changeTone(MelodyPlayer* melody);

  /**
   * Halt the advancement of the melody reproduction.
   * This is the shared method for pause and stop.
   */
  void haltPlay();

  /**
   * Configure pin to emit PWM.
   */
  void turnOn();

  /**
   * Disable PWM and put the buzzer is low-power state.
   * This calls will fails if PWM is not initialized!
   */
  void turnOff();
};

#endif  // END MELODY_PLAYER_H
