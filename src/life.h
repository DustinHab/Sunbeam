// What moves through the image: a ship at the horizon, a sailing boat, an
// aircraft, gulls. Decides when one of them appears and where it is at each
// moment; the renderer draws it. All of it is rare: most of the time the image
// is empty.

#pragma once
#include <cstdint>
#include <vector>

// The models there are, in the order of MODELS in render.cpp.
enum { MODEL_CONTAINER, MODEL_FREIGHTER, MODEL_DINGHY, MODEL_AIRLINER, MODEL_GULL, MODEL_COUNT };

// One of them at one moment.
struct Prop {
  int model = 0;
  double pos[3] = {};      // meters from the viewer: to the right, above the water (without water: above eye level), ahead
  float heading = 0;       // direction of travel in radians: 0 away from the viewer, then clockwise seen from above
  float pitch = 0;         // nose up
  float bank = 0;          // right side down
  float wing = 0;          // birds: angle of the inner wing against its rest position, upward
  float tip = 0;           // and of the outer wing against the inner one
  bool mirrored = false;   // the model reversed left to right
};

// What it depends on.
struct Surroundings {
  int ground = 0;               // 0 mountains, 1 skyline, 2 sea
  bool water = false;
  double eye = 0;               // eye above the water, meters
  double wind = 0;              // wind at the ground, meters per second
  double windX = 0, windZ = -1; // where it blows to: right, ahead
  double night = 0;             // 0 day, 1 night
  double rain = 0;              // 0 to 1
  double halfWidth = 0.45;      // half the image width as a tangent
  double top = 0.4;             // the image reaches this far above the horizon, as a tangent
  double amount = 0.5;          // the slider: 0 nothing, 1 twice as often as at half
  bool has[MODEL_COUNT] = {};   // which models are there
  float length[MODEL_COUNT] = {};   // and how long they are, meters
};

class Life {
 public:
  // For still images and tests: exactly these are in the image and move on at
  // their velocity (meters per second); nothing else appears. flap: how strongly
  // a bird beats its wings, 0 to 1.
  struct Staged {
    Prop prop;
    double velocity[3] = {};
    double flap = 0;
  };
  void stage(const std::vector<Staged>& list);

  // dt seconds later: what is in the image now.
  const std::vector<Prop>& update(double dt, const Surroundings& s);

 private:
  // One that is under way.
  struct Actor {
    bool on = false;
    Prop prop;
    double from[3] = {};        // where it started
    double velocity[3] = {};
    double age = 0;
    double reach = 0;           // half its length plus margin: this far beyond the edge it is gone
    double course = 0;          // the heading of its straight course
    double lean = 0;            // a boat: how far the wind heels it, right side down
    // Birds: the path sways sideways and up and down, and beating alternates with
    // gliding. A boat rocks by the same numbers.
    double sway[2] = {}, swayEvery[2] = {1, 1}, swayAt[2] = {};
    double beat = 0, beatRate = 3.2;   // phase of the wing beat and beats per second
    double flap = 0;                   // how strongly it beats right now
    bool beating = false;
    double change = 0;                 // seconds until it switches between beating and gliding
  };

  double rnd();
  double rnd(double lo, double hi) { return lo + (hi - lo) * rnd(); }
  double pause(double mean);
  void reset(const Surroundings& s);
  void startShip(const Surroundings& s, bool midway);
  void startBoat(const Surroundings& s);
  void startPlane(const Surroundings& s);
  void startBirds(const Surroundings& s);
  bool gone(const Actor& a, const Surroundings& s) const;
  void travel(Actor& a, double dt);
  void fly(Actor& a, double dt, const Surroundings& s);

  uint32_t dice_ = 0;
  int ground_ = -1;
  Actor ship_, boat_, plane_, bird_[3];
  double shipIn_ = 0, boatIn_ = 0, planeIn_ = 0, birdsIn_ = 0;   // seconds until the next one may appear
  double time_ = 0;
  std::vector<Staged> staged_;
  bool fixed_ = false;
  std::vector<Prop> now_;
};
