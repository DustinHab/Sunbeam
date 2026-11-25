#include "life.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

const double PI = 3.14159265358979;
// Mean pause between two of a kind at half slider, seconds. A ship takes twenty
// minutes to half an hour to cross the image, a boat ten to fifteen minutes, an
// aircraft about one, gulls a quarter of a minute. With these pauses the sea is
// empty five sixths of the time (see --life).
const double SHIP_PAUSE = 3.5 * 3600, BOAT_PAUSE = 3 * 3600, PLANE_PAUSE = 45 * 60, BIRD_PAUSE = 20 * 60;
// A boat cannot sail closer to the wind than this.
const double CLOSE_HAULED = 48 * PI / 180;
// The model of the dinghy carries its sails on this side: 1 starboard, -1 port.
const int SAIL_SIDE = -1;
// The model of the gull holds its wings raised by about 34 degrees. Gliding, they
// are almost level: turned down by this much at the shoulder, the outer wing
// hanging slightly. A wing beat swings around that by BEAT at the shoulder and
// by TIP_BEAT further out.
const double WING_REST = -0.50, TIP_REST = -0.06, BEAT = 0.5, TIP_BEAT = 0.38;

double wrap(double a) {
  while (a > PI) a -= 2 * PI;
  while (a <= -PI) a += 2 * PI;
  return a;
}

}  // namespace

double Life::rnd() {
  if (!dice_) dice_ = uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
  dice_ ^= dice_ << 13, dice_ ^= dice_ >> 17, dice_ ^= dice_ << 5;
  return (dice_ & 0xffffff) / double(0x1000000);
}

// Time until the next one of a kind: around the mean, never right away. It runs
// down faster the higher the slider is (see update).
double Life::pause(double mean) { return mean * rnd(0.4, 1.6); }

void Life::stage(const std::vector<Staged>& list) {
  staged_ = list;
  fixed_ = true;
}

// A new place: nothing is under way. Not everything appears at once afterwards,
// each kind starts somewhere within its pause; only a ship may already be
// halfway, it is in the image for so long.
void Life::reset(const Surroundings& s) {
  ground_ = s.ground;
  ship_.on = boat_.on = plane_.on = false;
  for (Actor& b : bird_) b.on = false;
  shipIn_ = pause(SHIP_PAUSE) * rnd(0.1, 1);
  boatIn_ = pause(BOAT_PAUSE) * rnd(0.1, 1);
  planeIn_ = pause(PLANE_PAUSE) * rnd(0.1, 1);
  birdsIn_ = pause(BIRD_PAUSE) * rnd(0.1, 1);
  if (s.ground == 2 && (s.has[MODEL_CONTAINER] || s.has[MODEL_FREIGHTER]) && s.amount >= 0.02 && rnd() < 0.1) startShip(s, true);
}

// A ship far out, close to the horizon. It crosses the image on a straight
// course that is slightly oblique, so that it slowly comes nearer or recedes.
void Life::startShip(const Surroundings& s, bool midway) {
  bool both = s.has[MODEL_CONTAINER] && s.has[MODEL_FREIGHTER];
  int model = both ? (rnd() < 0.5 ? MODEL_CONTAINER : MODEL_FREIGHTER) : s.has[MODEL_CONTAINER] ? MODEL_CONTAINER : MODEL_FREIGHTER;
  Actor& a = ship_;
  a = Actor();
  a.on = true;
  a.prop.model = model;
  double dir = rnd() < 0.5 ? -1 : 1;
  double z = rnd(7500, 15000);
  double speed = model == MODEL_CONTAINER ? rnd(8, 11) : rnd(5.5, 7.5);
  a.course = dir * (PI / 2 - rnd(-0.22, 0.22));
  a.velocity[0] = std::sin(a.course) * speed;
  a.velocity[2] = std::cos(a.course) * speed;
  a.reach = 0.5 * s.length[model] + 30;
  a.from[0] = midway ? rnd(-0.8, 0.8) * s.halfWidth * z : -dir * (s.halfWidth * z + a.reach);
  a.from[2] = z;
  a.prop.heading = float(a.course);
  travel(a, 0);
}

// A sailing dinghy, much nearer. Its course follows from the wind: across the
// image, but never closer to the wind than a boat can sail. The wind heels it to
// leeward, and the sail is on that side.
void Life::startBoat(const Surroundings& s) {
  Actor& a = boat_;
  a = Actor();
  a.on = true;
  a.prop.model = MODEL_DINGHY;
  double dir = rnd() < 0.5 ? -1 : 1;
  // At times near enough to make out its crew; nearer still it would sit below the
  // bottom edge of the image.
  double z = s.ground == 1 ? rnd(550, 1400) : rnd(420, 1800);
  double heading = dir * (PI / 2 - rnd(-0.3, 0.3));
  double upwind = std::atan2(-s.windX, -s.windZ);
  double off = wrap(heading - upwind);
  if (std::abs(off) < CLOSE_HAULED) {
    off = off < 0 ? -CLOSE_HAULED : CLOSE_HAULED;
    heading = wrap(upwind + off);
  }
  a.course = heading;
  // Fastest with the wind from the side, slower close to it.
  double speed = std::clamp(0.42 * s.wind, 1.2, 3.4) * (0.75 + 0.25 * std::abs(std::sin(off)));
  a.velocity[0] = std::sin(heading) * speed;
  a.velocity[2] = std::cos(heading) * speed;
  dir = a.velocity[0] < 0 ? -1 : 1;
  a.reach = 0.5 * s.length[MODEL_DINGHY] + 6;
  a.from[0] = -dir * (s.halfWidth * z + a.reach);
  a.from[2] = z;
  // Leeward is the side the wind blows to.
  bool starboard = s.windX * std::cos(heading) - s.windZ * std::sin(heading) > 0;
  a.lean = (starboard ? 1 : -1) * std::min(s.wind * s.wind / 250, 0.32) * std::abs(std::sin(off));
  a.prop.mirrored = (starboard ? 1 : -1) != SAIL_SIDE;
  a.prop.heading = float(heading);
  a.swayEvery[0] = rnd(2.9, 3.8), a.swayEvery[1] = rnd(3.9, 4.8);
  a.swayAt[0] = rnd(0, 2 * PI), a.swayAt[1] = rnd(0, 2 * PI);
  travel(a, 0);
}

// An airliner a few kilometers away, low enough to be recognized: climbing out
// or on its approach, on a straight course.
void Life::startPlane(const Surroundings& s) {
  Actor& a = plane_;
  a = Actor();
  a.on = true;
  a.prop.model = MODEL_AIRLINER;
  double dir = rnd() < 0.5 ? -1 : 1;
  double z = s.ground == 0 ? rnd(6000, 9000) : rnd(3500, 7000);
  double rise = rnd(0.10, std::max(0.12, std::min(0.34, s.top - 0.05)));
  double speed = rnd(95, 130), climb = rnd(-0.045, 0.06);
  a.course = dir * (PI / 2 - rnd(-0.5, 0.5));
  a.velocity[0] = std::sin(a.course) * std::cos(climb) * speed;
  a.velocity[1] = std::sin(climb) * speed;
  a.velocity[2] = std::cos(a.course) * std::cos(climb) * speed;
  a.reach = 0.5 * s.length[MODEL_AIRLINER] + 20;
  a.from[0] = -dir * (s.halfWidth * z + a.reach);
  a.from[1] = s.eye + z * rise;
  a.from[2] = z;
  a.prop.heading = float(a.course);
  // The nose points slightly above the flight path.
  a.prop.pitch = float(climb + 0.03);
  travel(a, 0);
}

// One to three gulls that cross the image, mostly at some distance, now and
// then close by. The others follow the first one, staggered behind and beside
// it.
void Life::startBirds(const Surroundings& s) {
  double pick = rnd();
  int count = pick < 0.55 ? 1 : pick < 0.85 ? 2 : 3;
  bool nearby = rnd() < 0.15;
  double z = nearby ? rnd(70, 120) : rnd(130, 380);
  double y = std::max(s.eye + z * rnd(nearby ? 0.02 : -0.01, 0.2), 2.5);
  double dir = rnd() < 0.5 ? -1 : 1;
  double course = dir * (PI / 2 - rnd(-0.3, 0.3)), speed = rnd(9, 13);
  double fx = std::sin(course), fz = std::cos(course);
  // The wind carries them along, a strong one not fully: they hold against it.
  double carry = 0.5 * std::min(s.wind, 8.0);
  for (int i = 0; i < 3; i++) {
    Actor& a = bird_[i];
    a = Actor();
    if (i >= count) continue;
    a.on = true;
    a.prop.model = MODEL_GULL;
    a.course = course;
    double back = i ? i * rnd(3, 8) : 0, side = i ? rnd(-5, 5) : 0, up = i ? rnd(-2, 2) : 0;
    a.velocity[0] = fx * speed + s.windX * carry;
    a.velocity[2] = fz * speed + s.windZ * carry;
    a.reach = 3 + back;
    a.from[0] = -dir * (s.halfWidth * z + 3) - fx * back + fz * side;
    a.from[1] = std::max(y + up, 2.0);
    a.from[2] = z - fz * back - fx * side;
    a.sway[0] = rnd(1.5, 4.5), a.swayEvery[0] = rnd(6, 11), a.swayAt[0] = rnd(0, 2 * PI);
    a.sway[1] = rnd(0.5, 1.8), a.swayEvery[1] = rnd(5, 9), a.swayAt[1] = rnd(0, 2 * PI);
    a.beatRate = rnd(2.9, 3.5);
    a.beat = rnd(0, 2 * PI);
    a.beating = rnd() < 0.6;
    a.flap = a.beating ? 1 : 0;
    a.change = rnd(1, 4);
    fly(a, 0, s);
  }
}

// It has left the image on the side it was heading for, or has come too close
// or gone too far for its kind.
bool Life::gone(const Actor& a, const Surroundings& s) const {
  const double* p = a.prop.pos;
  if (a.age > 3 * 3600) return true;
  int m = a.prop.model;
  double nearest = m == MODEL_GULL ? 15 : m == MODEL_DINGHY ? 200 : m == MODEL_AIRLINER ? 1500 : 4000;
  double farthest = m == MODEL_GULL ? 900 : m == MODEL_DINGHY ? 4500 : m == MODEL_AIRLINER ? 16000 : 22000;
  if (p[2] < nearest || p[2] > farthest) return true;
  double edge = s.halfWidth * p[2] + a.reach;
  if ((a.velocity[0] >= 0 && p[0] > edge) || (a.velocity[0] < 0 && p[0] < -edge)) return true;
  // Out through the top.
  return (p[1] - s.eye) / p[2] > s.top + 0.04;
}

// Straight ahead.
void Life::travel(Actor& a, double dt) {
  a.age += dt;
  for (int i = 0; i < 3; i++) a.prop.pos[i] = a.from[i] + a.velocity[i] * a.age;
  a.prop.speed = float(std::hypot(a.velocity[0], a.velocity[2]));
}

// A gull: its straight course sways sideways and up and down, it banks into the
// turns, and it alternates between beating its wings and gliding; in more wind
// it glides longer.
void Life::fly(Actor& a, double dt, const Surroundings& s) {
  a.age += dt;
  double fx = std::sin(a.course), fz = std::cos(a.course);
  double w0 = 2 * PI / a.swayEvery[0], w1 = 2 * PI / a.swayEvery[1];
  double p0 = w0 * a.age + a.swayAt[0], p1 = w1 * a.age + a.swayAt[1];
  double side = a.sway[0] * std::sin(p0), up = a.sway[1] * std::sin(p1);
  a.prop.pos[0] = a.from[0] + a.velocity[0] * a.age + fz * side;
  a.prop.pos[1] = std::max(a.from[1] + up, 1.5);
  a.prop.pos[2] = a.from[2] + a.velocity[2] * a.age - fx * side;
  double sideV = a.sway[0] * w0 * std::cos(p0), upV = a.sway[1] * w1 * std::cos(p1);
  double vx = a.velocity[0] + fz * sideV, vz = a.velocity[2] - fx * sideV;
  a.prop.heading = float(std::atan2(vx, vz));
  a.prop.pitch = float(std::atan2(upV, std::hypot(vx, vz)));
  // Acceleration to the right tilts the right wing down.
  a.prop.bank = float(std::atan(-a.sway[0] * w0 * w0 * std::sin(p0) / 9.81));

  a.change -= dt;
  if (a.change <= 0) {
    a.beating = !a.beating;
    a.change = a.beating ? rnd(1.2, 3.5) : rnd(1.0, 4.0) * (1 + s.wind / 10);
  }
  a.flap += std::clamp((a.beating ? 1.0 : 0.0) - a.flap, -dt / 0.35, dt / 0.35);
  a.beat = std::fmod(a.beat + dt * a.beatRate * 2 * PI, 2 * PI);
  // The outer wing lags behind the inner one.
  a.prop.wing = float(WING_REST + a.flap * BEAT * std::sin(a.beat));
  a.prop.tip = float(TIP_REST + a.flap * TIP_BEAT * std::sin(a.beat - 1.0));
}

const std::vector<Prop>& Life::update(double dt, const Surroundings& s) {
  now_.clear();
  time_ += dt;
  if (fixed_) {
    for (Staged& g : staged_) {
      for (int i = 0; i < 3; i++) g.prop.pos[i] += g.velocity[i] * dt;
      Prop p = g.prop;
      p.speed = float(std::hypot(g.velocity[0], g.velocity[2]));
      double beat = time_ * 3.2 * 2 * PI;
      p.wing += float(WING_REST + g.flap * BEAT * std::sin(beat));
      p.tip += float(TIP_REST + g.flap * TIP_BEAT * std::sin(beat - 1.0));
      if (p.model >= 0 && p.model < MODEL_COUNT && s.has[p.model]) now_.push_back(p);
    }
    return now_;
  }
  if (s.ground != ground_) reset(s);
  if (s.amount < 0.02) {
    ship_.on = boat_.on = plane_.on = false;
    for (Actor& b : bird_) b.on = false;
    return now_;
  }
  // The pauses run down faster the higher the slider is.
  double passed = dt * s.amount / 0.5;

  // Ships only on the open sea.
  if (ship_.on) {
    travel(ship_, dt);
    if (gone(ship_, s) || s.ground != 2) {
      ship_.on = false;
      shipIn_ = pause(SHIP_PAUSE);
    }
  } else if (s.ground == 2 && (s.has[MODEL_CONTAINER] || s.has[MODEL_FREIGHTER])) {
    shipIn_ -= passed;
    if (shipIn_ <= 0) startShip(s, false);
  }

  // A dinghy sails by day, in a wind that carries it and does not overpower it,
  // and not in rain, fog or cold.
  if (boat_.on) {
    travel(boat_, dt);
    // It rocks in the waves.
    boat_.prop.pitch = float(0.02 * std::sin(2 * PI * boat_.age / boat_.swayEvery[0] + boat_.swayAt[0]));
    boat_.prop.bank = float(boat_.lean + 0.015 * std::sin(2 * PI * boat_.age / boat_.swayEvery[1] + boat_.swayAt[1]));
    if (gone(boat_, s)) {
      boat_.on = false;
      boatIn_ = pause(BOAT_PAUSE);
    }
  } else if (s.water && s.has[MODEL_DINGHY] && s.night < 0.25 && s.rain < 0.15 && s.wind > 2.0 && s.wind < 10.5 && s.fog < 0.3 && s.temperature > 4) {
    boatIn_ -= passed;
    if (boatIn_ <= 0) startBoat(s);
  }

  if (plane_.on) {
    travel(plane_, dt);
    if (gone(plane_, s)) {
      plane_.on = false;
      planeIn_ = pause(PLANE_PAUSE);
    }
  } else if (s.has[MODEL_AIRLINER]) {
    planeIn_ -= passed;
    if (planeIn_ <= 0) startPlane(s);
  }

  // Gulls over water, by day, not in heavy rain, storm or dense fog.
  bool flying = false;
  for (Actor& b : bird_) {
    if (!b.on) continue;
    fly(b, dt, s);
    if (gone(b, s)) b.on = false;
    flying |= b.on;
  }
  if (!flying) {
    if (bird_[0].age > 0) {
      for (Actor& b : bird_) b = Actor();
      birdsIn_ = pause(BIRD_PAUSE);
    } else if (s.water && s.has[MODEL_GULL] && s.night < 0.2 && s.rain < 0.35 && s.wind < 14 && s.fog < 0.55) {
      birdsIn_ -= passed;
      if (birdsIn_ <= 0) startBirds(s);
    }
  }

  if (ship_.on) now_.push_back(ship_.prop);
  if (boat_.on) now_.push_back(boat_.prop);
  if (plane_.on) now_.push_back(plane_.prop);
  for (const Actor& b : bird_) {
    if (b.on) now_.push_back(b.prop);
  }
  return now_;
}
