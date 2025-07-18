#ifndef MELODY_H
#define MELODY_H

#include <Arduino.h>

#include <memory>
#include <vector>

/**
 * A note and its duration.
 */
struct NoteDuration {
  // The note frequency.
  unsigned short frequency;
  // The note duration. The representation can be either relative (fixed-point, decimal
  // part = 1 bit) to time units or absolute (in milliseconds)
  unsigned short duration;
};

/**
 * This class stores the data melody (notes and metadata).
 * To ease the creation of a melody, you should use MelodyFactory class.
 */
class Melody {
public:
  Melody() : notes(nullptr){};

  Melody(String title, unsigned short timeUnit, std::shared_ptr<std::vector<NoteDuration>> notes, bool automaticSilence)
    : title(title), timeUnit(timeUnit), notes(notes), automaticSilence(automaticSilence){};

  /**
   * Return the title of the melody.
   */
  String getTitle() const {
    return title;
  };

  /**
   * Return the time unit (i.e. the minimum length of a note), in milliseconds.
   */
  unsigned short getTimeUnit() const {
    return timeUnit;
  };

  /**
   * Get the number of notes.
   */
  unsigned short getLength() const {
    if (notes == nullptr) return 0;
    return (*notes).size();
  }

  /**
   * Get the note at the given position.
   * If the melody or the position is invalid, return a zeroed NoteDuration.
   */
  NoteDuration getNote(unsigned short i) const {
    if (i < getLength()) { return (*notes)[i]; }
    return { 0, 0 };
  }

  /**
   * Return true if the melody should be played with a small delay between each note.
   */
  bool getAutomaticSilence() const {
    return automaticSilence;
  }

  /**
   * Return true if the melody is valid, false otherwise.
   */
  bool isValid() const {
    return notes != nullptr && (*notes).size() != 0;
  }

  /**
   * Return true if the melody is valid, false otherwise.
   */
  explicit operator bool() const {
    return isValid();
  }

private:
  String title;
  // in milliseconds
  unsigned short timeUnit;
  std::shared_ptr<std::vector<NoteDuration>> notes;
  const static unsigned short maxLength = 1000;
  bool automaticSilence;

  // Enable debug messages over serial port
  const static bool debug = false;
};

#endif  // END MELODY_H
