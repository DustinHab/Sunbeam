// Where sun and moon are in the image: by their true course or set by hand.

#pragma once
#include "astro.h"
#include "render.h"

// By the true course. The image looks to where the body is highest (south in the
// northern hemisphere): it rises at the left edge, moves across the sky and sets
// at the right one. How high the arc reaches follows the true altitude, flat in
// winter, in summer almost up to the top edge.
Heaven heavenOfBodies(const Bodies& b, double latitude, const Renderer& r);

// Set by hand, in logical pixels. The moon is opposite the sun: when the sun
// sinks below the horizon, the moon rises on the other side. Its phase is
// today's.
Heaven heavenOfHand(float sunX, float sunY, const Bodies& b, const Renderer& r);

// The night sky. The planets are placed like sun and moon, each on its own arc
// from edge to edge; the stars turn as a whole behind them.
Night nightOf(const Stars& s, double latitude, const Renderer& r);

// A sun set by hand has no time of its own. Its stars are those of the moment,
// within twelve hours of now, at which the real sun is as high as the one in the
// image: in the evening if it is on the side where it sets, else in the morning.
double timeOfHand(double now, float sunX, float sunY, double latitude, double longitude, const Renderer& r);

// The inverse of that: where the sun belongs if the moon is to be at this spot.
void sunOfMoon(float moonX, float moonY, const Renderer& r, float& sunX, float& sunY);
