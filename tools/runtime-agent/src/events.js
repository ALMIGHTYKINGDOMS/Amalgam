// Durable runtime-agent event collector.
// Buffers telemetry locally, retries writes, and requeues failed batches.

import crypto from "node:crypto";

export class EventCollector {
  constructor(supabase, config, nodeId) {
    this.supabase = supabase;
    this.config = config;
    this.nodeId = nodeId;
    this.source = "runtime";
    this.sourceVersion = "0.2.0";
    this.eventBuffer = [];
    this.metricBuffer = [];
    this.errorBuffer = [];
    this.usageBuffer = [];
    this.flushIntervalMs = (config.heartbeatIntervalSec || 30) * 1000;
    this.maxBufferSize = 500;
    this.maxRetainedBuffer = 5000;
    this.running = false;
    this.flushPromise = null;
  }

  start() {
    if (this.running) return;
    this.running = true;
    void this._flushLoop();
    console.log(`[events] Collector started (flush every ${this.flushIntervalMs / 1000}s)`);
  }

  async stop() {
    this.running = false;
    await this.flush();
  }

  event(category, eventName, properties = {}, options = {}) {
    this.eventBuffer.push({
      source: this.source,
      source_version: this.sourceVersion,
      source_node_id: this.nodeId,
      user_id: options.userId || null,
      session_id: options.sessionId || "",
      category,
      event_name: eventName,
      properties,
      duration_ms: options.durationMs ?? null,
      value: options.value ?? null,
      page: options.page || "",
      screen: options.screen || "",
      user_agent: `amalgam-runtime/${this.sourceVersion}`,
    });
    this._maybeFlush();
  }

  metric(name, value, options = {}) {
    // Telemetry callers use null for a genuinely unobservable measurement.
    // Do not coerce null, an empty string, or a boolean into zero: that turns
    // "unknown" into a misleading healthy-looking datapoint.
    if (typeof value !== "number" || !Number.isFinite(value)) return false;
    this.metricBuffer.push({
      source: this.source,
      source_node_id: this.nodeId,
      user_id: options.userId || null,
      metric_name: name,
      metric_type: options.type || "gauge",
      value,
      unit: options.unit || "",
      tags: options.tags || {},
    });
    this._maybeFlush();
    return true;
  }

  error(errorType, message, options = {}) {
    const safeMessage = String(message || "unknown error").slice(0, 4000);
    const fingerprint = crypto.createHash("sha256")
      .update(`${errorType}:${safeMessage}:${options.component || ""}`)
      .digest("hex");
    this.errorBuffer.push({
      source: this.source,
      source_version: this.sourceVersion,
      source_node_id: this.nodeId,
      user_id: options.userId || null,
      error_type: String(errorType || "runtime").slice(0, 128),
      error_code: String(options.code || "").slice(0, 128),
      severity: options.severity || "error",
      message: safeMessage,
      stack_trace: String(options.stackTrace || "").slice(0, 16000),
      component: String(options.component || "").slice(0, 128),
      operation: String(options.operation || "").slice(0, 128),
      properties: options.properties || {},
      fingerprint,
    });
    this._maybeFlush();
  }

  usage(featureName, options = {}) {
    this.usageBuffer.push({
      user_id: options.userId || null,
      source: this.source,
      feature_name: String(featureName || "unknown").slice(0, 128),
      feature_category: String(options.category || "").slice(0, 128),
      usage_count: Math.max(1, Number(options.count || 1)),
      duration_ms: options.durationMs ?? null,
      success: options.success !== false,
      properties: options.properties || {},
    });
    this._maybeFlush();
  }

  serverStarted(serverId, serverName, pid) {
    this.event("server", "server_started", { server_id: serverId, server_name: serverName, pid });
    this.usage("server_start", { category: "server" });
  }
  serverStopped(serverId, serverName, reason) {
    this.event("server", "server_stopped", { server_id: serverId, server_name: serverName, reason });
    this.usage("server_stop", { category: "server" });
  }
  serverError(serverId, serverName, errorMessage) {
    this.error("server", errorMessage, { component: "ServerProcessManager", operation: "server_lifecycle", properties: { server_id: serverId, server_name: serverName } });
  }
  operationStarted(operationId, operationType, serverId) {
    this.event("server", "operation_started", { operation_id: operationId, operation_type: operationType, server_id: serverId });
  }
  operationCompleted(operationId, operationType, success, durationMs) {
    this.event("server", "operation_completed", { operation_id: operationId, operation_type: operationType, success }, { durationMs });
    this.usage(`operation_${operationType}`, { category: "server", durationMs, success });
  }
  heartbeatSent(nodeId, metrics) {
    this.metric("heartbeat.cpu_pct", metrics.cpuUsagePct, { unit: "%" });
    this.metric("heartbeat.memory_used_mb", metrics.memoryUsedMb, { unit: "MB" });
    this.metric("heartbeat.storage_used_gb", metrics.storageUsedGb, { unit: "GB" });
    this.metric("heartbeat.server_count", metrics.serverCount || 0);
  }
  telemetryReported(serverId, telemetry) {
    for (const [name, value, unit] of [
      ["server.tps", telemetry.tps, "tps"], ["server.mspt", telemetry.mspt, "ms"],
      ["server.players", telemetry.playerCount, ""], ["server.entities", telemetry.entityCount, ""],
      ["server.heap_used_mb", telemetry.heapUsedMb, "MB"], ["server.uptime_ms", telemetry.uptimeMs, "ms"],
    ]) this.metric(name, value, { unit, tags: { server_id: serverId } });
  }
  registrationSucceeded(nodeId, reregistered) {
    this.event("server", "node_registered", { node_id: nodeId, reregistered });
    this.usage("node_register", { category: "server" });
  }
  registrationFailed(error) {
    this.error("server", error, { component: "SupabaseClient", operation: "node_registration", severity: "fatal" });
  }
  networkError(operation, error) {
    this.error("network", error, { component: "SupabaseClient", operation });
  }
  consoleFlushed(serverId, lineCount) {
    this.event("server", "console_flushed", { server_id: serverId, line_count: lineCount });
  }

  _maybeFlush() {
    if (this.eventBuffer.length + this.metricBuffer.length + this.errorBuffer.length + this.usageBuffer.length >= this.maxBufferSize) {
      void this.flush();
    }
  }

  async flush() {
    if (this.flushPromise) return this.flushPromise;
    this.flushPromise = this._flushNow().finally(() => { this.flushPromise = null; });
    return this.flushPromise;
  }

  async _flushNow() {
    const batches = [
      ["insert_events", { p_events: this.eventBuffer.splice(0) }, this.eventBuffer],
      ["insert_metrics", { p_metrics: this.metricBuffer.splice(0) }, this.metricBuffer],
      ["insert_errors", { p_errors: this.errorBuffer.splice(0) }, this.errorBuffer],
      ["insert_feature_usage", { p_usage: this.usageBuffer.splice(0) }, this.usageBuffer],
    ].filter(([, args]) => Object.values(args)[0].length > 0);

    const results = await Promise.all(batches.map(([fn, args]) => this._flushWithRetry(fn, args)));
    results.forEach((success, index) => {
      if (success) return;
      const [, args, target] = batches[index];
      const payload = Object.values(args)[0];
      target.unshift(...payload);
      if (target.length > this.maxRetainedBuffer) target.splice(this.maxRetainedBuffer);
    });
  }

  async _flushWithRetry(functionName, args) {
    for (let attempt = 0; attempt <= 2; attempt++) {
      try {
        const { error } = await this.supabase.rpc(functionName, args);
        if (!error) return true;
        if (attempt === 2) {
          console.error(`[events] ${functionName} failed after retries: ${error.message}`);
          return false;
        }
        await delay(2000 * 2 ** attempt);
      } catch (error) {
        if (attempt === 2) {
          console.error(`[events] ${functionName} failed after retries: ${error.message}`);
          return false;
        }
        await delay(2000 * 2 ** attempt);
      }
    }
    return false;
  }

  async _flushLoop() {
    while (this.running) {
      await delay(this.flushIntervalMs);
      if (this.running) await this.flush();
    }
  }
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
