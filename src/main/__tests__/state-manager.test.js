const { StateManager } = require('../state-manager');
const config = require('../config');

// Helper: create a StateManager with a controllable clock. `ref.t` is advanced
// manually in tests to drive timeout/reminder logic without real timers.
function createState(ref) {
  return new StateManager({ now: () => ref.t });
}

function makeEvent(sessionId, name, extra = {}) {
  return {
    session_id: sessionId,
    hook_event_name: name,
    project_path: 'D:\\proj',
    project_name: 'proj',
    ...extra,
  };
}

// ===== A. event -> state mapping (single session) =====
describe('A. event -> state mapping', () => {
  it('SessionStart -> idle, overall sleeping', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'SessionStart'));
    expect(sm.overallState).toBe('sleeping');
    expect(sm.getSnapshot().sessions[0].status).toBe('idle');
  });

  it('UserPromptSubmit -> working', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'SessionStart'));
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    expect(sm.overallState).toBe('working');
    expect(sm.getSnapshot().sessions[0].status).toBe('working');
  });

  it('PreToolUse -> working and clears alert', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'X' }));
    expect(sm.overallState).toBe('alert');
    sm.handleHookEvent(makeEvent('s1', 'PreToolUse'));
    expect(sm.overallState).toBe('working');
    expect(sm.getSnapshot().sessions[0].alertMessage).toBeNull();
  });

  it('PostToolUse -> working (keeps confirmation-needed)', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'X' }));
    sm.handleHookEvent(makeEvent('s1', 'PostToolUse'));
    expect(sm.getSnapshot().sessions[0].status).toBe('confirmation-needed');
  });

  it('Notification + tool_name -> confirmation-needed / alert', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'RunCommand' }));
    expect(sm.overallState).toBe('alert');
    expect(sm.getSnapshot().sessions[0].status).toBe('confirmation-needed');
    expect(sm.getSnapshot().sessions[0].alertMessage).toContain('RunCommand');
  });

  it('Notification + task_complete -> idle (completion whitelist)', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { notification_type: 'task_complete' }));
    expect(sm.getSnapshot().sessions[0].status).toBe('idle');
    expect(sm.overallState).toBe('sleeping');
  });

  it('Notification with confirm keyword -> alert', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { message: '请确认是否执行' }));
    expect(sm.getSnapshot().sessions[0].status).toBe('confirmation-needed');
  });

  it('Notification ambiguous -> idle (ALERT_ON_AMBIGUOUS_NOTIFICATION=false)', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification'));
    expect(sm.getSnapshot().sessions[0].status).toBe('idle');
    expect(sm.overallState).toBe('sleeping');
  });

  it('Stop -> idle', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Stop'));
    expect(sm.getSnapshot().sessions[0].status).toBe('idle');
    expect(sm.overallState).toBe('sleeping');
  });
});

// ===== B. multi-session priority =====
describe('B. multi-session priority', () => {
  it('alert takes priority over working and idle', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s2', 'Notification', { tool_name: 'X' }));
    expect(sm.overallState).toBe('alert');
  });

  it('overall reverts to working when alert clears', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s2', 'Notification', { tool_name: 'X' }));
    expect(sm.overallState).toBe('alert');
    sm.handleHookEvent(makeEvent('s2', 'Stop'));
    expect(sm.overallState).toBe('working');
  });

  it('snapshot sorts confirmation-needed first, then working, then idle', () => {
    const sm = createState({ t: 1000 });
    sm.handleHookEvent(makeEvent('s1', 'SessionStart'));              // idle
    sm.handleHookEvent(makeEvent('s2', 'UserPromptSubmit'));          // working
    sm.handleHookEvent(makeEvent('s3', 'Notification', { tool_name: 'X' })); // confirmation
    const order = sm.getSnapshot().sessions.map((s) => s.status);
    expect(order).toEqual(['confirmation-needed', 'working', 'idle']);
  });
});

// ===== C. timeout cleanup (clock-injected) =====
describe('C. timeout cleanup', () => {
  it('working past WORKING_TIMEOUT -> idle', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    expect(sm.overallState).toBe('working');
    ref.t = 1000 + config.WORKING_TIMEOUT + 1;
    sm.cleanupStaleSessions();
    expect(sm.getSnapshot().sessions[0].status).toBe('idle');
    expect(sm.overallState).toBe('sleeping');
  });

  it('session past SESSION_TIMEOUT -> removed', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    sm.handleHookEvent(makeEvent('s1', 'SessionStart'));
    ref.t = 1000 + config.SESSION_TIMEOUT + 1;
    sm.cleanupStaleSessions();
    expect(sm.getSnapshot().sessions.length).toBe(0);
  });

  it('confirmation-needed past WORKING_TIMEOUT -> idle (alert cleared)', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'X' }));
    expect(sm.overallState).toBe('alert');
    ref.t = 1000 + config.WORKING_TIMEOUT + 1;
    sm.cleanupStaleSessions();
    expect(sm.getSnapshot().sessions[0].status).toBe('idle');
    expect(sm.getSnapshot().sessions[0].alertMessage).toBeNull();
    expect(sm.overallState).toBe('sleeping');
  });

  it('not expired -> no update emit', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    let updateCount = 0;
    sm.on('update', () => updateCount++);
    updateCount = 0; // discard the initial
    ref.t = 1000 + 100;
    sm.cleanupStaleSessions();
    expect(updateCount).toBe(0);
  });
});

// ===== D. alert trigger and re-remind =====
describe('D. alert trigger and re-remind', () => {
  it('entering alert emits alert exactly once', () => {
    const sm = createState({ t: 1000 });
    let alertCount = 0;
    sm.on('alert', () => alertCount++);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'X' }));
    expect(alertCount).toBe(1);
  });

  it('re-remind before ALERT_REMINDER does not emit', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    let alertCount = 0;
    sm.on('alert', () => alertCount++);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'X' }));
    expect(alertCount).toBe(1);
    ref.t = 1000 + config.ALERT_REMINDER - 10;
    sm._checkAndRemindAlert();
    expect(alertCount).toBe(1);
  });

  it('re-remind at/after ALERT_REMINDER emits again', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    let alertCount = 0;
    sm.on('alert', () => alertCount++);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'X' }));
    expect(alertCount).toBe(1);
    ref.t = 1000 + config.ALERT_REMINDER;
    sm._checkAndRemindAlert();
    expect(alertCount).toBe(2);
    ref.t = 1000 + config.ALERT_REMINDER * 2;
    sm._checkAndRemindAlert();
    expect(alertCount).toBe(3);
  });

  it('no remind after alert cleared', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    let alertCount = 0;
    sm.on('alert', () => alertCount++);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'X' }));
    sm.handleHookEvent(makeEvent('s1', 'Stop'));
    expect(sm.overallState).toBe('sleeping');
    ref.t = 1000 + config.ALERT_REMINDER * 5;
    sm._checkAndRemindAlert();
    expect(alertCount).toBe(1);
  });
});

// ===== E. dirty check =====
describe('E. dirty check', () => {
  it('repeated identical event emits update only once', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    let updateCount = 0;
    sm.on('update', () => updateCount++);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    const first = updateCount;
    ref.t = 1100; // time advances but meaningful fields unchanged
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    expect(updateCount).toBe(first);
  });

  it('alertMessage change emits update', () => {
    const ref = { t: 1000 };
    const sm = createState(ref);
    sm.handleHookEvent(makeEvent('s1', 'UserPromptSubmit'));
    let updateCount = 0;
    sm.on('update', () => updateCount++);
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'A' }));
    expect(updateCount).toBe(1);
    updateCount = 0;
    sm.handleHookEvent(makeEvent('s1', 'Notification', { tool_name: 'B' }));
    expect(updateCount).toBe(1);
  });

  it('first call always emits (baseline)', () => {
    const sm = createState({ t: 1000 });
    let updateCount = 0;
    sm.on('update', () => updateCount++);
    sm.handleHookEvent(makeEvent('s1', 'SessionStart'));
    expect(updateCount).toBe(1);
  });
});

// ===== F. config sanity =====
describe('F. config', () => {
  it('exposes expected port and timeouts', () => {
    expect(config.PORT).toBe(17521);
    expect(config.WORKING_TIMEOUT).toBe(3 * 60 * 1000);
    expect(config.SESSION_TIMEOUT).toBe(10 * 60 * 1000);
    expect(config.ALERT_REMINDER).toBe(60 * 1000);
  });

  it('ALERT_ON_AMBIGUOUS_NOTIFICATION defaults false', () => {
    expect(config.ALERT_ON_AMBIGUOUS_NOTIFICATION).toBe(false);
  });

  it('HOOK_EVENTS covers all six lifecycle events', () => {
    expect(config.HOOK_EVENTS).toEqual([
      'SessionStart', 'UserPromptSubmit', 'PreToolUse', 'PostToolUse', 'Stop', 'Notification',
    ]);
  });
});
