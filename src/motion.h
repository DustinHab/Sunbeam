// Follows the mouse pointer and a window while it is dragged, and says where
// both will be a few milliseconds from now.
//
// The position of a window reaches the light late: Windows draws the window at
// its new place in the same refresh in which the light still shows the old one.
// A dragged window follows the pointer, though: its program moves it in steps,
// each time to where the pointer was a short time before, at a fixed distance.
// That relation and the pace of the steps are measured here while the window
// moves; the place of the window at the moment the next frame appears then
// follows from the path of the pointer, which is known or nearly so.

#pragma once
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

// The refreshes of the display as Windows composes its image.
struct Beat {
  double base = 0, period = 1.0 / 60, asked = -1e9;
  // Asks Windows anew, at most four times a second. t: now, in seconds of the
  // performance counter.
  void update(double t);
  // The first refresh at or after t.
  double next(double t) const;
};

// While a window is dragged or the pointer moves, a frame is made for every
// display refresh, and it starts as late as it can: this long before the
// refresh at which Windows takes it over. The lead follows what the graphics
// card needs: the longest it took over the last frames, and a little more.
struct Lead {
  double value = 0.0032;   // seconds
  // A frame has to be finished this long before its refresh.
  static constexpr double ROOM = 0.00025;
  // A frame that starts this much later than planned still counts as on time;
  // one that starts later is made for the refresh after.
  static constexpr double SLACK = 0.0003;
  // took: seconds from the start of a frame until the graphics card was through
  // with it; negative: it was not through by its refresh.
  void judge(double took, double period);

 private:
  double past_[48] = {};
  int count_ = 0, at_ = 0;
};

// Tells Windows how this program ranks on the graphics card. aside: it only
// gets what the others leave, so that a game in front keeps its frame rate.
void standAside(bool aside);

// Waits until a time of the performance counter, to a fraction of a
// millisecond.
class Snooze {
 public:
  Snooze();
  ~Snooze();
  static double now();          // seconds
  // early: the timer is set this much ahead of the time, the rest is spent
  // spinning. A thread is not always woken on the dot: the more that is covered
  // this way, the more punctual the wait.
  void until(double when, double early = 0.00015);

 private:
  HANDLE timer_ = nullptr;
};

// Where the readings come from: the real desktop, or a simulated one for checks.
struct MotionSource {
  virtual ~MotionSource() = default;
  virtual double now() = 0;                             // seconds, the clock everything here is measured by
  virtual bool pointer(POINT& at, bool& held) = 0;      // its position, and whether the primary button is held
  virtual HWND dragged(bool held) = 0;                  // the window being moved or sized right now, if any
  virtual bool frame(HWND window, RECT& frame) = 0;     // its visible frame on the screen
};

class Motion {
 public:
  ~Motion() { stop(); }
  // source nullptr: the real desktop.
  void start(MotionSource* source = nullptr);
  void stop();
  // Readings are only taken while this is on.
  void enable(bool on);
  // A window has moved: it is followed for a while even if no button is held.
  void note(HWND window);

  struct Guess {
    HWND window = nullptr;   // the window that has moved lately; nullptr: none
    RECT frame{};            // where its frame will be
    bool led = false;        // computed ahead from the pointer; otherwise as last read
    bool moving = false;     // it is moving right now
    POINT pointer{};         // where the pointer will be
    double px = 0, py = 0;   // the same, to fractions of a pixel
    bool held = false;       // the primary button is held
    bool stirred = false;    // the pointer has moved in the last moments
    float speed = 0;         // of the pointer, pixels per second
  };
  // Where the window will be at the time `window`, and the pointer at the time
  // `pointer` (both by the clock of the source).
  Guess guess(double window, double pointer);
  // Where the frame of the followed window was at that time; false if it is not
  // known.
  bool frameAt(double time, HWND window, RECT& frame);
  // Where the pointer was at that time, as read; false if it is not known.
  bool pointerWas(double time, POINT& at);
  // Seconds between two reports of the mouse if it reports seldom and evenly;
  // otherwise 0.
  double reports();

  // How the window currently follows the pointer, for the log.
  struct Model {
    bool valid = false;
    HWND window = nullptr;
    double delay = 0;         // seconds between the program reading the pointer and the window being there
    double offset[4] = {};    // edge minus pointer coordinate: left, top, right, bottom
    bool rides[4] = {};       // this edge follows the pointer
    double spread = 0;        // how far the edges stray from that relation, pixels
    double step = 0;          // seconds between two steps of the window
    bool regular = false;     // the steps come evenly and can be counted ahead
  };
  Model model();

 private:
  struct Sample {
    double t = 0;
    POINT c{};
    bool held = false;
    HWND w = nullptr;
    RECT f{};
  };
  // The path of the pointer over the last moments: a parabola through its
  // readings.
  struct Path {
    double t0 = 0;                 // times count from here: the last reading
    double x[3] = {}, y[3] = {};   // position there, speed, half the acceleration
    // A mouse that reports seldom: seconds between two reports, and the time of
    // one of them. every 0: it reports densely, or not evenly.
    double every = 0, first = 0;
  };
  void run();
  void fit(double t);
  bool pace(double& every, double& first) const;
  Path path() const;
  size_t firstAt(double t) const;
  bool pointerAt(double t, double& x, double& y, const Path* ahead = nullptr) const;

  MotionSource* src_ = nullptr;
  bool ownSource_ = false;
  std::thread thread_;
  std::atomic<bool> quit_{false}, on_{false};
  HANDLE wake_ = nullptr;
  std::mutex lock_;
  std::vector<Sample> ring_;   // the last readings, oldest first from head_
  size_t head_ = 0, count_ = 0;
  HWND noted_ = nullptr;
  double notedAt_ = -1e9;
  double movedAt_ = -1e9;      // when the frame of the followed window last changed
  double stirredAt_ = -1e9;    // when the pointer last moved
  Model model_;
};
