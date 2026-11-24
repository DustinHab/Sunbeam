#include "motion.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <iterator>

// From gdi32: the class in which the graphics card serves a process. 0 idle, 2
// normal.
extern "C" LONG APIENTRY D3DKMTSetProcessSchedulingPriorityClass(HANDLE process, int priority);

void standAside(bool aside) { D3DKMTSetProcessSchedulingPriorityClass(GetCurrentProcess(), aside ? 0 : 2); }

namespace {

const size_t RING = 1024;          // readings kept: a second at the fast pace
const double FAST = 0.001;         // seconds between two readings while something may move
const double SLOW = 0.008;         // and while nothing does
const double ASK_EVERY = 0.008;    // how often it is asked which window is being dragged
const double NOTE_LIFE = 0.5;      // a window that has moved without a button is followed this long
const double MOVING = 0.15;        // a frame that changed this recently counts as moving
const double STIRRED = 0.05;       // and a pointer that did
const double FIT_EVERY = 0.008;    // how often the relation to the pointer is measured anew
const double FIT_SPAN = 0.16;      // over the readings of this many seconds
const double DELAY_MAX = 0.06;     // the window is searched for at most this far behind the pointer
const double AHEAD_MAX = 0.02;     // the pointer is extended along its path at most this far into the future
// Steps at least this far apart that come evenly are counted ahead one by one;
// faster or uneven ones are taken as a steady flow.
const double STEP_EVEN = 0.003;

class Desktop : public MotionSource {
 public:
  Desktop() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    freq_ = double(f.QuadPart);
  }
  double now() override {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / freq_;
  }
  bool pointer(POINT& at, bool& held) override {
    held = GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) < 0;
    return GetCursorPos(&at) != 0;
  }
  HWND dragged(bool held) override {
    // The window Windows itself is moving or sizing; failing that, with the button
    // held, the one in front: some programs move their windows by themselves.
    GUITHREADINFO gi = {sizeof(gi)};
    if (GetGUIThreadInfo(0, &gi) && (gi.flags & GUI_INMOVESIZE) && gi.hwndMoveSize) return GetAncestor(gi.hwndMoveSize, GA_ROOT);
    if (!held) return nullptr;
    HWND front = GetForegroundWindow();
    return front ? GetAncestor(front, GA_ROOT) : nullptr;
  }
  bool frame(HWND window, RECT& frame) override {
    if (!IsWindow(window)) return false;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) return true;
    return GetWindowRect(window, &frame) != 0;
  }

 private:
  double freq_ = 1;
};

// The lead: no shorter than this, and this much longer than the frames took.
const double LEAD_MIN = 0.0012, LEAD_SPARE = 0.0006;

double edgeOf(const RECT& r, int k) { return double(k == 0 ? r.left : k == 1 ? r.top : k == 2 ? r.right : r.bottom); }

}  // namespace

void Beat::update(double t) {
  if (t - asked < 0.25) return;
  asked = t;
  DWM_TIMING_INFO ti = {};
  ti.cbSize = sizeof(ti);
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || !ti.qpcRefreshPeriod) return;
  double p = double(ti.qpcRefreshPeriod) / double(f.QuadPart);
  if (p < 0.001 || p > 0.05) return;
  period = p;
  base = double(ti.qpcVBlank) / double(f.QuadPart);
}

double Beat::next(double t) const { return base + std::ceil((t - base) / period) * period; }

void Lead::judge(double took, double period) {
  // A frame that was too late needed more than it had: how much more is not
  // known, a little is assumed.
  past_[at_] = took < 0 ? value - LEAD_SPARE + 0.0005 : took;
  at_ = (at_ + 1) % int(std::size(past_));
  count_ = std::min(count_ + 1, int(std::size(past_)));
  double most = 0;
  for (int i = 0; i < count_; i++) most = std::max(most, past_[i]);
  value = std::clamp(most + LEAD_SPARE, LEAD_MIN, 2 * period);
}

Snooze::Snooze() {
  // A timer that keeps to fractions of a millisecond (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION); on systems
  // without one, the ordinary kind.
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
  if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

Snooze::~Snooze() {
  if (timer_) CloseHandle(timer_);
}

double Snooze::now() {
  static const double freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) / freq;
}

void Snooze::until(double when, double early) {
  for (;;) {
    double left = when - now();
    if (left <= 0) return;
    if (left < early + 0.00005 || !timer_) {
      // The last stretch by spinning: no timer is that exact.
      while (now() < when) YieldProcessor();
      return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG((left - early) * 1e7);
    if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) return;
    WaitForSingleObject(timer_, 200);
  }
}

void Motion::start(MotionSource* source) {
  stop();
  ownSource_ = source == nullptr;
  src_ = source ? source : new Desktop();
  ring_.assign(RING, Sample());
  head_ = count_ = 0;
  model_ = Model();
  quit_ = false;
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  thread_ = std::thread([this] { run(); });
}

void Motion::stop() {
  if (thread_.joinable()) {
    quit_ = true;
    SetEvent(wake_);
    thread_.join();
  }
  if (wake_) CloseHandle(wake_);
  wake_ = nullptr;
  if (ownSource_) delete src_;
  src_ = nullptr;
  ownSource_ = false;
}

void Motion::enable(bool on) {
  if (on_.exchange(on) != on && wake_) SetEvent(wake_);
}

void Motion::note(HWND window) {
  if (!src_) return;
  std::lock_guard<std::mutex> hold(lock_);
  noted_ = window;
  notedAt_ = src_->now();
}

void Motion::run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  // A timer that keeps to fractions of a millisecond; on systems without one, the
  // ordinary kind.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  HWND target = nullptr;
  bool wasHeld = false;
  POINT wasAt = {LONG_MIN, LONG_MIN};
  double asked = -1e9, fitted = -1e9;
  RECT before{};
  bool haveBefore = false;
  while (!quit_) {
    if (!on_) {
      WaitForSingleObject(wake_, 500);
      target = nullptr;
      haveBefore = false;
      continue;
    }
    Sample s;
    s.t = src_->now();
    src_->pointer(s.c, s.held);
    bool busy;
    {
      std::lock_guard<std::mutex> hold(lock_);
      if (s.c.x != wasAt.x || s.c.y != wasAt.y) stirredAt_ = s.t;
      wasAt = s.c;
      if (s.t - asked >= ASK_EVERY || s.held != wasHeld) {
        asked = s.t;
        HWND now = src_->dragged(s.held);
        if (!now && s.t - notedAt_ < NOTE_LIFE) now = noted_;
        if (now != target) {
          target = now;
          haveBefore = false;
          model_ = Model();
        }
      }
      wasHeld = s.held;
      if (target && src_->frame(target, s.f)) {
        s.w = target;
        if (haveBefore && !EqualRect(&before, &s.f)) movedAt_ = s.t;
        before = s.f;
        haveBefore = true;
      } else {
        haveBefore = false;
      }
      ring_[(head_ + count_) % RING] = s;
      if (count_ < RING) count_++;
      else head_ = (head_ + 1) % RING;
      busy = s.held || s.t - movedAt_ < NOTE_LIFE || s.t - stirredAt_ < NOTE_LIFE;
      if (s.w && s.t - movedAt_ < MOVING && s.t - fitted >= FIT_EVERY) {
        fitted = s.t;
        fit(s.t);
      }
    }
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG((busy ? FAST : SLOW) * 1e7);
    HANDLE both[2] = {timer, wake_};
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForMultipleObjects(2, both, FALSE, 100);
    else WaitForSingleObject(wake_, busy ? 1 : 8);
  }
  if (timer) CloseHandle(timer);
}

// The pointer at a time within the readings (between two of them along a
// straight line), or shortly after the last one along its path.
bool Motion::pointerAt(double t, double& x, double& y) const {
  if (!count_) return false;
  auto at = [&](size_t i) -> const Sample& { return ring_[(head_ + i) % RING]; };
  const Sample& last = at(count_ - 1);
  if (t >= last.t) {
    double vx, vy;
    speedNow(vx, vy);
    double ahead = std::min(t - last.t, AHEAD_MAX);
    x = last.c.x + vx * ahead;
    y = last.c.y + vy * ahead;
    return true;
  }
  if (t < at(0).t) return false;
  size_t lo = 0, hi = count_ - 1;
  while (hi - lo > 1) {
    size_t mid = (lo + hi) / 2;
    if (at(mid).t <= t) lo = mid;
    else hi = mid;
  }
  const Sample &a = at(lo), &b = at(hi);
  double f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0;
  x = a.c.x + (b.c.x - a.c.x) * f;
  y = a.c.y + (b.c.y - a.c.y) * f;
  return true;
}

// The speed of the pointer over the last hundredth of a second.
void Motion::speedNow(double& vx, double& vy) const {
  vx = vy = 0;
  if (count_ < 2) return;
  auto at = [&](size_t i) -> const Sample& { return ring_[(head_ + i) % RING]; };
  const Sample& last = at(count_ - 1);
  size_t i = count_ - 1;
  while (i > 0 && last.t - at(i).t < 0.010) i--;
  const Sample& first = at(i);
  double dt = last.t - first.t;
  if (dt < 0.004 || dt > 0.05) return;
  vx = (last.c.x - first.c.x) / dt;
  vy = (last.c.y - first.c.y) / dt;
}

// Measures how the followed window hangs on the pointer. A program moves its
// window in steps: at each step it puts the window where the pointer was a
// short time before, at a fixed distance. That time and the distance of each
// edge are measured at the steps of the last moments, along with the pace of
// the steps. Called under the lock.
void Motion::fit(double t) {
  auto at = [&](size_t i) -> const Sample& { return ring_[(head_ + i) % RING]; };
  HWND window = at(count_ - 1).w;
  Model m;
  m.window = window;
  // The readings of the last moments in which the button was held and this
  // window was followed.
  size_t first = count_;
  while (first > 0 && t - at(first - 1).t <= FIT_SPAN && at(first - 1).w == window && at(first - 1).held) first--;
  if (count_ - first < 24) {
    model_ = m;
    return;
  }
  // The steps: the readings at which the frame had changed. The step itself lies
  // between that reading and the one before.
  struct Step {
    double t;
    size_t i;
  };
  Step steps[160];
  int n = 0;
  double lo[4], hi[4], path = 0;
  for (int k = 0; k < 4; k++) lo[k] = 1e18, hi[k] = -1e18;
  for (size_t i = first; i < count_; i++) {
    for (int k = 0; k < 4; k++) {
      double v = edgeOf(at(i).f, k);
      lo[k] = std::min(lo[k], v), hi[k] = std::max(hi[k], v);
    }
    if (i == first) continue;
    path += std::hypot(double(at(i).c.x - at(i - 1).c.x), double(at(i).c.y - at(i - 1).c.y));
    if (!EqualRect(&at(i).f, &at(i - 1).f) && n < int(std::size(steps))) steps[n++] = {0.5 * (at(i).t + at(i - 1).t), i};
  }
  double speed = path / std::max(at(count_ - 1).t - at(first).t, 1e-3);
  int lead = 0;
  for (int k = 1; k < 4; k++) {
    if (hi[k] - lo[k] > hi[lead] - lo[lead]) lead = k;
  }
  if (n < 5 || hi[lead] - lo[lead] < 6) {
    model_ = m;
    return;
  }
  // Their pace, and whether they come evenly.
  double span = (steps[n - 1].t - steps[0].t) / (n - 1), uneven = 0;
  for (int j = 1; j < n; j++) uneven += std::abs(steps[j].t - steps[j - 1].t - span);
  m.step = span;
  m.regular = span >= STEP_EVEN && uneven / (n - 1) < 0.2 * span;
  // How an edge strays from the pointer if the program reads the pointer this
  // long before it moves the window: the mean distance and the scatter around it.
  auto stray = [&](int k, double delay, double& mean) {
    double sum = 0, sq = 0;
    int used = 0;
    for (int j = 0; j < n; j++) {
      double x, y;
      if (!pointerAt(steps[j].t - delay, x, y)) continue;
      double d = edgeOf(at(steps[j].i).f, k) - (k % 2 == 0 ? x : y);
      sum += d, sq += d * d;
      used++;
    }
    if (used < 4) return 1e9;
    mean = sum / used;
    return std::sqrt(std::max(sq / used - mean * mean, 0.0));
  };
  double best = 1e9, mean = 0;
  for (double delay = 0; delay <= DELAY_MAX + 1e-9; delay += 0.001) {
    double s = stray(lead, delay, mean);
    if (s < best) best = s, m.delay = delay;
  }
  // On a straight stretch every delay fits about equally well. The one found
  // before stays unless another fits clearly better.
  if (model_.window == window && model_.valid) {
    double kept = stray(lead, model_.delay, mean);
    if (kept <= 1.25 * best + 0.25) m.delay = model_.delay;
  }
  // An edge rides on the pointer if it strays from it by no more than the window
  // travels in two milliseconds, or one and a half pixels.
  double allow = std::max(1.5, 0.002 * speed);
  for (int k = 0; k < 4; k++) {
    if (hi[k] - lo[k] < 1.5) continue;
    double s = stray(k, m.delay, mean);
    m.rides[k] = s <= allow;
    m.offset[k] = mean;
    if (m.rides[k]) m.spread = std::max(m.spread, s);
  }
  m.valid = m.rides[lead];
  model_ = m;
}

Motion::Guess Motion::guess(double window, double pointer) {
  Guess g;
  std::lock_guard<std::mutex> hold(lock_);
  if (!count_) return g;
  const Sample& last = ring_[(head_ + count_ - 1) % RING];
  double x = last.c.x, y = last.c.y, vx, vy;
  pointerAt(pointer, x, y);
  speedNow(vx, vy);
  g.pointer = {LONG(std::lround(x)), LONG(std::lround(y))};
  g.held = last.held;
  g.stirred = last.t - stirredAt_ < STIRRED;
  g.speed = float(std::hypot(vx, vy));
  if (!last.w || last.t - movedAt_ > NOTE_LIFE) return g;
  g.window = last.w;
  g.frame = last.f;
  g.moving = last.t - movedAt_ < MOVING;
  if (!g.moving || !last.held || !model_.valid || model_.window != last.w) return g;
  // The step that will have been the last one at that time. Even steps can be
  // counted ahead: if none is due until then, the window stays where it was
  // read. Otherwise the last step is, on average, half a step back.
  double step = window - 0.5 * model_.step;
  if (model_.regular) {
    double due = std::floor((window - movedAt_) / model_.step);
    g.led = true;
    if (due < 1) return g;
    step = movedAt_ + due * model_.step;
  }
  // Each edge that rides on the pointer is where the pointer was the delay before
  // that step, plus its distance.
  double px, py;
  if (!pointerAt(step - model_.delay, px, py)) return g;
  LONG* edge[4] = {&g.frame.left, &g.frame.top, &g.frame.right, &g.frame.bottom};
  double reach = 8 + 0.03 * g.speed;
  for (int k = 0; k < 4; k++) {
    if (!model_.rides[k]) continue;
    double v = (k % 2 == 0 ? px : py) + model_.offset[k];
    // Never far from where the edge was last read: if the relation no longer holds,
    // the reading wins.
    if (std::abs(v - *edge[k]) > reach) continue;
    *edge[k] = LONG(std::lround(v));
    g.led = true;
  }
  return g;
}

bool Motion::frameAt(double time, HWND window, RECT& frame) {
  std::lock_guard<std::mutex> hold(lock_);
  for (size_t i = count_; i > 0; i--) {
    const Sample& s = ring_[(head_ + i - 1) % RING];
    if (s.t > time) continue;
    if (s.w != window || time - s.t > 0.02) return false;
    frame = s.f;
    return true;
  }
  return false;
}

Motion::Model Motion::model() {
  std::lock_guard<std::mutex> hold(lock_);
  return model_;
}
