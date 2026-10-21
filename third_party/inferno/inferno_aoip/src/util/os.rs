use thread_priority::{thread_native_id, Error};

#[cfg(all(target_family = "unix", not(target_os = "macos")))]
pub fn set_current_thread_realtime(priority_hint: u8) -> Result<(), Error> {
  use thread_priority::unix::set_thread_priority_and_policy;
  use thread_priority::{
    RealtimeThreadSchedulePolicy, ThreadPriority, ThreadPriorityValue, ThreadSchedulePolicy,
  };

  set_thread_priority_and_policy(
    thread_native_id(),
    ThreadPriority::Crossplatform(ThreadPriorityValue::try_from(priority_hint).unwrap()),
    ThreadSchedulePolicy::Realtime(RealtimeThreadSchedulePolicy::Fifo),
  )
}

#[cfg(not(target_family = "unix"))]
pub fn set_current_thread_realtime(_priority_hint: u32) -> Result<(), Error> {
  thread_priority::set_current_thread_priority(ThreadPriority::Max)
}

/// Jose Virtual Soundcard addition: on macOS, POSIX real-time priority isn't
/// available to normal apps, so audio threads were scheduled like any other
/// thread and sometimes woke several milliseconds late (heard as clicks).
/// Use a Mach time-constraint policy instead, as Core Audio does for its
/// own I/O threads: about 0.3 ms of work every 1 ms.
#[cfg(target_os = "macos")]
pub fn set_current_thread_realtime(_priority_hint: u8) -> Result<(), Error> {
  #[repr(C)]
  struct TimebaseInfo {
    numer: u32,
    denom: u32,
  }
  #[repr(C)]
  struct TimeConstraintPolicy {
    period: u32,
    computation: u32,
    constraint: u32,
    preemptible: i32,
  }
  extern "C" {
    fn mach_timebase_info(info: *mut TimebaseInfo) -> i32;
    fn mach_thread_self() -> u32;
    fn thread_policy_set(thread: u32, flavor: u32, policy_info: *mut i32, count: u32) -> i32;
  }
  const THREAD_TIME_CONSTRAINT_POLICY: u32 = 2;
  const THREAD_TIME_CONSTRAINT_POLICY_COUNT: u32 = 4;

  let mut tb = TimebaseInfo { numer: 0, denom: 0 };
  unsafe { mach_timebase_info(&mut tb) };
  if tb.numer == 0 {
    return Err(Error::OS(-1));
  }
  // nanoseconds -> mach absolute time units
  let abs = |ns: u64| (ns * tb.denom as u64 / tb.numer as u64) as u32;
  let mut policy = TimeConstraintPolicy {
    period: abs(1_000_000),
    computation: abs(300_000),
    constraint: abs(1_000_000),
    preemptible: 1,
  };
  let r = unsafe {
    thread_policy_set(
      mach_thread_self(),
      THREAD_TIME_CONSTRAINT_POLICY,
      &mut policy as *mut TimeConstraintPolicy as *mut i32,
      THREAD_TIME_CONSTRAINT_POLICY_COUNT,
    )
  };
  if r == 0 {
    log::info!("audio thread is real-time");
    Ok(())
  } else {
    log::warn!("couldn't make audio thread real-time (Mach error {r})");
    Err(Error::OS(r))
  }
}
