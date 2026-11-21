// A stepped setting's next value (the picture settings, finding 275: Brightness, Contrast,
// Saturation, Gamma), as the game's SETTINGS page steps them with left/right: one choice that way,
// stopping at the ends (null: nothing to do). A value that is not one of the choices (a number
// written in dhd.ini) goes to the next choice that way; one that is no number counts as the middle
// choice (the neutral one, as the renderer reads it).
'use strict';

function stepChoice(values, value, d) {
  const at = values.indexOf(value);
  if (at >= 0) {
    const next = at + d;
    return next >= 0 && next < values.length ? values[next] : null;
  }
  if (values.length === 0) return null;
  const v = value === '' || value == null ? NaN : Number(value);
  if (!Number.isFinite(v)) return stepChoice(values, values[values.length >> 1], d);
  const way = d > 0 ? values.filter((c) => Number(c) > v) : values.filter((c) => Number(c) < v);
  return way.length ? way[d > 0 ? 0 : way.length - 1] : null;
}

if (typeof module !== 'undefined') module.exports = { stepChoice };
