// A Face laid out natively for a 128x64 1-bit panel.
//
// The stock m5avatar::Face is built for a 320x240 canvas (eyes at x=90/230,
// mouth at y=148) and the library's own faces/OledFace.h is the same
// coordinate space despite the name. Driving that on a 128x64 OLED means
// either rendering a 320x240 sprite and throwing 87% of it away, or using
// Avatar::setScale() to shrink it — which on a 1-bit panel turns the eyes
// into ragged blobs, because there is no grey to antialias the downscale
// into. So the geometry below is authored at 1:1 for this panel instead.
//
// Every position is the CENTRE of its part, not a corner: Eye reads
// rect.getCenterX/Y(), Mouth and Eyeblow read getLeft/getTop(), and the
// two-argument BoundingRect(top, left) leaves width/height at 0 so those
// agree. Note the argument order is (top, left) — y first.
#pragma once

#include <Avatar.h>

namespace m5avatar {

// Eyes sit on y=30 rather than the geometric centre (32) to leave room for
// the mouth, which grows downward from y=50 when it opens.
class SmallOledFace : public Face {
 public:
  SmallOledFace()
      : Face(new Mouth(20, 36, 3, 18), new BoundingRect(50, 64),
             new Eye(9, false), new BoundingRect(30, 40),
             new Eye(9, true), new BoundingRect(30, 88),
             new Eyeblow(18, 3, false), new BoundingRect(14, 40),
             new Eyeblow(18, 3, true), new BoundingRect(14, 88),
             new BoundingRect(0, 0, 128, 64),
             new M5Canvas(&M5.Lcd), new M5Canvas(&M5.Lcd)) {}
};

}  // namespace m5avatar
