//! What a Vulkan submit's VkTimelineSemaphoreSubmitInfo carries (finding 201): once it is chained,
//! both value lists have one entry per semaphore, so a driver that reads pWaitSemaphoreValues[i]
//! for every wait semaphore (PowerVR's proprietary driver, issue #82) reads a value, not null.
#![cfg(all(feature = "native", any(windows, target_os = "linux", target_os = "android")))]

use ash::vk::{self, Handle};
use wgpu_hal::vulkan::{SemaphoreList, SemaphoreListMode, SemaphoreType};

fn sem(n: u64) -> vk::Semaphore {
    vk::Semaphore::from_raw(n)
}

/// The submit's counts and values: (wait semaphores, signal semaphores, the timeline info if
/// chained as (wait values, signal values)).
type Seen = (u32, u32, Option<(Vec<u64>, Vec<u64>)>);

fn submit(waits: &[SemaphoreType], signals: &[SemaphoreType]) -> Seen {
    let mut wait = SemaphoreList::new(SemaphoreListMode::Wait);
    let mut signal = SemaphoreList::new(SemaphoreListMode::Signal);
    for w in waits {
        wait.push_wait(copy(w), vk::PipelineStageFlags::TOP_OF_PIPE);
    }
    for s in signals {
        signal.push_signal(copy(s));
    }
    let mut timeline = std::mem::MaybeUninit::uninit();
    let info = SemaphoreList::add_to_submit(&mut wait, &mut signal, vk::SubmitInfo::default(), &mut timeline);
    let chained = if info.p_next.is_null() {
        None
    } else {
        // SAFETY: add_to_submit chains exactly `timeline`, alive (and written) here.
        let t = unsafe { &*info.p_next.cast::<vk::TimelineSemaphoreSubmitInfo>() };
        assert_eq!(t.s_type, vk::StructureType::TIMELINE_SEMAPHORE_SUBMIT_INFO);
        let read = |p: *const u64, n: u32| {
            if n == 0 {
                Vec::new()
            } else {
                assert!(!p.is_null(), "{n} values at a null pointer");
                // SAFETY: the list's own values, n of them, alive with `wait`/`signal`.
                unsafe { std::slice::from_raw_parts(p, n as usize) }.to_vec()
            }
        };
        Some((read(t.p_wait_semaphore_values, t.wait_semaphore_value_count),
              read(t.p_signal_semaphore_values, t.signal_semaphore_value_count)))
    };
    (info.wait_semaphore_count, info.signal_semaphore_count, chained)
}

fn copy(s: &SemaphoreType) -> SemaphoreType {
    match *s {
        SemaphoreType::Binary(raw) => SemaphoreType::Binary(raw),
        SemaphoreType::Timeline(raw, v) => SemaphoreType::Timeline(raw, v),
    }
}

/// Issue #82's submit: the swapchain's acquire (or the relay) is a binary wait, the fence a
/// timeline signal. The wait list gets its value (0) instead of count 0 and a null pointer.
#[test]
fn binary_wait_with_timeline_signal_has_a_wait_value() {
    let seen = submit(&[SemaphoreType::Binary(sem(1))],
                      &[SemaphoreType::Binary(sem(2)), SemaphoreType::Binary(sem(3)), SemaphoreType::Timeline(sem(4), 7)]);
    assert_eq!(seen, (1, 3, Some((vec![0], vec![!0, !0, 7]))));
}

/// Two binary waits (acquire and relay) and a timeline wait: one value each, the timeline's kept.
#[test]
fn mixed_waits_keep_the_timeline_value() {
    let seen = submit(&[SemaphoreType::Binary(sem(1)), SemaphoreType::Timeline(sem(2), 5), SemaphoreType::Binary(sem(3))],
                      &[SemaphoreType::Timeline(sem(4), 9)]);
    assert_eq!(seen, (3, 1, Some((vec![0, 5, 0], vec![9]))));
}

/// The first submit: nothing to wait for, so the wait list stays empty.
#[test]
fn no_waits_stay_empty() {
    let seen = submit(&[], &[SemaphoreType::Binary(sem(1)), SemaphoreType::Timeline(sem(2), 1)]);
    assert_eq!(seen, (0, 2, Some((vec![], vec![!0, 1]))));
}

/// A device without timeline semaphores (fences instead): nothing is chained, as before.
#[test]
fn binary_only_chains_nothing() {
    let seen = submit(&[SemaphoreType::Binary(sem(1))], &[SemaphoreType::Binary(sem(2))]);
    assert_eq!(seen, (1, 1, None));
}
