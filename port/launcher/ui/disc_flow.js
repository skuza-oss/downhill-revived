// A phone's disc import as the page follows it (finding 225): the native side checks the chosen
// file, copies it into the app and checks the copy (Android: DiscImport.java), sending
// "disc-import" events { job, stage, ... }; this is the page's state for them. Each pick is a job:
// only the newest job's events count (an older one's late progress or problem is dropped), and
// once a job ended (done, kept, problem) its late progress is dropped too.
//
// Stages: idle; checking (the file, maybe downloaded first by a cloud provider); copying { done,
// total } (total 0: unknown); done { deletable, size } (copied and checked; deletable: the provider
// lets the original be deleted, so the page asks); kept (the question answered, or none asked);
// problem { problem } (a coded { code, params } or the native side's words).
'use strict';

const DISC_STAGES = ['idle', 'checking', 'copying', 'done', 'kept', 'problem'];

function discIdle() {
  return { stage: 'idle', job: 0 };
}

// While a job checks or copies, the disc can't be chosen again and the game can't start.
function discBusy(s) {
  return s.stage === 'checking' || s.stage === 'copying';
}

// The state after event e (the same object when e changes nothing).
function discFlow(s, e) {
  if (!e || !DISC_STAGES.includes(e.stage) || e.stage === 'idle' || typeof e.job !== 'number') return s;
  if (e.job < s.job) return s;                      // an older pick's
  const ended = s.stage === 'done' || s.stage === 'kept' || s.stage === 'problem';
  if (e.job === s.job && ended && e.stage !== 'kept') return s;  // a finished job's late events
  switch (e.stage) {
    case 'checking':
      return { stage: 'checking', job: e.job };
    case 'copying': {
      const done = Math.max(0, Number(e.done) || 0), total = Math.max(0, Number(e.total) || 0);
      if (s.job === e.job && s.stage === 'copying' && done < s.done) return s;  // never backwards
      return { stage: 'copying', job: e.job, done, total };
    }
    case 'done':
      return { stage: 'done', job: e.job, deletable: !!e.deletable, size: Number(e.size) || 0 };
    case 'kept':
      return s.job === e.job && s.stage === 'done' ? { stage: 'kept', job: e.job } : s;
    case 'problem':
      return { stage: 'problem', job: e.job, problem: e.problem ?? null };
  }
  return s;
}

if (typeof module !== 'undefined') module.exports = { DISC_STAGES, discIdle, discBusy, discFlow };
