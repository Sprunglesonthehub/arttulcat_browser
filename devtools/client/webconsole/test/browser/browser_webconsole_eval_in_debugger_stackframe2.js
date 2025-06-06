/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

// Test to make sure that web console commands can fire while paused at a
// breakpoint that was triggered from a JS call.  Relies on asynchronous js
// evaluation over the protocol - see Bug 1088861.

"use strict";

const TEST_URI =
  "http://example.com/browser/devtools/client/webconsole/" +
  "test/browser/test-eval-in-stackframe.html";

add_task(async function () {
  // TODO: Remove this pref change when middleware for terminating requests
  // when closing a panel is implemented
  await pushPref("devtools.debugger.features.inline-preview", false);

  info("open the console");
  const hud = await openNewTabAndConsole(TEST_URI);

  info("open the debugger");
  await openDebugger();

  const toolbox = hud.toolbox;
  const dbg = createDebuggerContext(toolbox);

  // firstCall calls secondCall, which has a debugger statement, so we'll be paused.
  const onFirstCallMessageReceived = waitForMessageByType(
    hud,
    "undefined",
    ".result"
  );

  const unresolvedSymbol = Symbol();
  let firstCallEvaluationResult = unresolvedSymbol;
  onFirstCallMessageReceived.then(message => {
    firstCallEvaluationResult = message;
  });
  execute(hud, "firstCall()");

  info("Waiting for a frame to be added");
  await waitForPaused(dbg, null, { shouldWaitForInlinePreviews: false });

  info("frames added, select the console again");
  await openConsole();

  info("Executing basic command while paused");
  await executeAndWaitForResultMessage(hud, "1 + 2", "3");
  ok(true, "`1 + 2` was evaluated whith debugger paused");

  info("Executing command using scoped variables while paused");
  await executeAndWaitForResultMessage(
    hud,
    "foo + foo2",
    `"globalFooBug783499foo2SecondCall"`
  );
  ok(true, "`foo + foo2` was evaluated as expected with debugger paused");

  info(
    "Checking the first command, which is the last to resolve since it paused"
  );
  Assert.strictEqual(
    firstCallEvaluationResult,
    unresolvedSymbol,
    "firstCall was not evaluated yet"
  );

  info("Resuming the thread");
  dbg.actions.resume();

  await onFirstCallMessageReceived;
  Assert.notStrictEqual(
    firstCallEvaluationResult,
    unresolvedSymbol,
    "firstCall() returned correct value"
  );
});
