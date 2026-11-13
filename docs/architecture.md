# Architecture

Outline. Fill in as the interfaces take shape.

## Components
- `PatternSource`: produces the next pattern, optionally using `Feedback` from the last capture
- `Display`: reports output dimensions and presents a `Frame`
- `Camera`: locks controls, grabs frames
- `ScanRunner`: pattern -> show -> settle -> grab -> store

`Frame` contains a width, height, and tightly packed RGB8 pixel buffer.
Display backends implement `Display::present(const Frame&)` and may scale the
frame to their output dimensions.

## Scan flow
TODO: sequence for one pattern, including settle time and dropped stale frames.

## Adding a new scan type
TODO: which interface to implement, how it is registered with the apps.

## Adding a new backend
TODO
