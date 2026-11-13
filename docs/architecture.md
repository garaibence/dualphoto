# Architecture

Outline. Fill in as the interfaces take shape.

## Components
- `PatternSource`: produces the next pattern, optionally using `Feedback` from the last capture
- `Display`: shows a pattern, reports when it is actually on screen
- `Camera`: locks controls, grabs frames
- `ScanRunner`: pattern -> show -> settle -> grab -> store

## Scan flow
TODO: sequence for one pattern, including settle time and dropped stale frames.

## Adding a new scan type
TODO: which interface to implement, how it is registered with the apps.

## Adding a new backend
TODO
