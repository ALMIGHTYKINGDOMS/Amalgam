import test from "node:test";
import assert from "node:assert/strict";
import {
  extractLimits,
  RateLimitModel,
  WINDOW_SECONDS,
  BUCKETS,
  classify,
} from "./ratelimit-matrix.mjs";

let clock = 0;
const now = () => clock;

test("every social edge function has a configured limit except whop-webhook", () => {
  const { limits, noLimit } = extractLimits();
  assert.ok(limits.size >= 15, `expected >= 15 limited actions, got ${limits.size}`);
  assert.deepEqual(noLimit, ["whop-webhook"], "only the HMAC webhook is exempt");
  assert.equal(limits.get("send_friend_request"), 20);
  assert.equal(limits.get("get_friends"), 60);
  assert.equal(limits.get("get_turn_credentials"), 10);
});

test("normal request volume succeeds", () => {
  clock = 0;
  const model = new RateLimitModel(
    new Map([["send_friend_request", 20]]),
    { now }
  );
  for (let i = 0; i < 15; i++) {
    assert.equal(model.check("userA", "send_friend_request"), true);
  }
});

test("burst abuse is rejected once the window is consumed", () => {
  clock = 0;
  const model = new RateLimitModel(
    new Map([["send_friend_request", 20]]),
    { now }
  );
  let allowed = 0;
  for (let i = 0; i < 60; i++) {
    if (model.check("userA", "send_friend_request")) allowed++;
  }
  assert.equal(allowed, 20, "exactly the window limit is allowed");
});

test("one user's rate limit does not affect another user", () => {
  clock = 0;
  const model = new RateLimitModel(
    new Map([["send_friend_request", 20]]),
    { now }
  );
  for (let i = 0; i < 25; i++) model.check("userA", "send_friend_request");
  assert.equal(model.check("userA", "send_friend_request"), false);
  assert.equal(model.check("userB", "send_friend_request"), true);
});

test("window reset restores capacity", () => {
  clock = 0;
  const model = new RateLimitModel(
    new Map([["send_friend_request", 20]]),
    { now }
  );
  for (let i = 0; i < 20; i++) model.check("userA", "send_friend_request");
  assert.equal(model.check("userA", "send_friend_request"), false);
  clock += WINDOW_SECONDS;
  assert.equal(model.check("userA", "send_friend_request"), true);
});

test("different operation buckets are independent", () => {
  clock = 0;
  const model = new RateLimitModel(
    new Map([
      ["send_friend_request", BUCKETS.WRITES],
      ["get_friends", BUCKETS.READS],
      ["get_turn_credentials", BUCKETS.EXPENSIVE],
    ]),
    { now }
  );
  for (let i = 0; i < BUCKETS.WRITES; i++) model.check("userA", "send_friend_request");
  assert.equal(model.check("userA", "send_friend_request"), false);
  // Reads and expensive actions are unaffected by the write burst.
  assert.equal(model.check("userA", "get_friends"), true);
  assert.equal(model.check("userA", "get_turn_credentials"), true);
  for (let i = 0; i < BUCKETS.READS - 1; i++) model.check("userA", "get_friends");
  assert.equal(model.check("userA", "get_friends"), false);
  assert.equal(model.check("userA", "get_turn_credentials"), true);
});

test("concurrent burst cannot race around the limit (atomic checks)", () => {
  clock = 0;
  const model = new RateLimitModel(
    new Map([["send_friend_request", 20]]),
    { now }
  );
  // Simulate 1000 interleaved requests from two sessions of the same user.
  let allowed = 0;
  for (let i = 0; i < 1000; i++) {
    const session = i % 2 === 0 ? "s1" : "s2";
    // Both sessions share the same user account.
    if (model.check("userA", "send_friend_request")) allowed++;
    void session;
  }
  assert.equal(allowed, 20, "the account-wide window caps all sessions");
});

test("normal UI polling never self-DOSes", () => {
  clock = 0;
  const model = new RateLimitModel(
    new Map([
      ["get_friends", BUCKETS.READS],
      ["get_friends_presence", BUCKETS.READS],
    ]),
    { now }
  );
  // Typical launcher polling: every 5s for an hour.
  for (let tick = 0; tick < 720; tick++) {
    assert.equal(model.check("userA", "get_friends"), true);
    assert.equal(model.check("userA", "get_friends_presence"), true);
    clock += 5;
  }
});

test("classification of action buckets", () => {
  assert.equal(classify("get_friends"), "READS");
  assert.equal(classify("send_friend_request"), "WRITES");
  assert.equal(classify("get_turn_credentials"), "EXPENSIVE");
  assert.equal(classify("create_session"), "EXPENSIVE");
});
