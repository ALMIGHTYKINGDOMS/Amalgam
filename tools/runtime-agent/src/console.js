// Console line streaming and telemetry reporting.
// Periodically flushes console output buffers and collects/process
// metrics from running Minecraft servers.

/**
 * Flush console line buffers to Supabase.
 * Call this periodically for each running server.
 */
export async function flushConsoleLines(supabase, nodeInfo, config, manager, eventCollector = null) {
  for (const [serverId, managed] of manager.servers) {
    if (!managed.process || managed.process.killed) continue;

    const buffer = managed.consoleBuffer;
    if (buffer.length === 0) continue;

    // Take lines that haven't been sent yet
    const unsent = managed.lastFlushedLine
      ? buffer.filter((l) => l.line_number > managed.lastFlushedLine)
      : buffer.slice(-100); // First flush: last 100 lines

    if (unsent.length === 0) continue;

    try {
      const { error } = await supabase.rpc("write_console_lines", {
        p_server_id: serverId,
        p_node_id: nodeInfo.id,
        p_node_secret: config.nodeSecret,
        p_lines: unsent.map((l) => ({
          line_number: l.line_number,
          level: l.level,
          source: l.source,
          content: l.content,
          raw: l.raw,
        })),
      });

      if (error) {
        console.error(`[console] Failed to write lines for ${serverId}:`, error.message);
      } else {
        managed.lastFlushedLine = unsent[unsent.length - 1].line_number;
        eventCollector?.consoleFlushed(serverId, unsent.length);
      }
    } catch (err) {
      console.error(`[console] Error flushing lines for ${serverId}:`, err.message);
    }
  }
}

/**
 * Collect telemetry from a running server by querying /status endpoint
 * or parsing the process output. Reports to Supabase.
 */
export async function collectAndReportTelemetry(supabase, nodeInfo, config, manager, eventCollector = null) {
  for (const [serverId, managed] of manager.servers) {
    if (!managed.process || managed.process.killed) continue;

    const uptimeMs = Date.now() - managed.startedAt;

    // Parse last console output for TPS, player count, etc.
    const recentLines = managed.consoleBuffer.slice(-50);
    const parsed = parseMinecraftMetrics(recentLines);

    try {
      const { error } = await supabase.rpc("write_telemetry", {
        p_server_id: serverId,
        p_node_id: nodeInfo.id,
        p_node_secret: config.nodeSecret,
        p_cpu_pct: 0, // Would need OS-level monitoring
        p_memory_used_mb: 0, // Would need jstat or JMX
        p_memory_max_mb: 0,
        p_memory_pct: 0,
        p_disk_used_mb: 0,
        p_thread_count: parsed.threadCount,
        p_heap_used_mb: parsed.heapUsedMb,
        p_heap_max_mb: parsed.heapMaxMb,
        p_gc_count: parsed.gcCount,
        p_gc_time_ms: parsed.gcTimeMs,
        p_tps: parsed.tps,
        p_mspt: parsed.mspt,
        p_entity_count: parsed.entityCount,
        p_player_count: parsed.playerCount,
        p_chunk_count: parsed.chunkCount,
        p_uptime_ms: uptimeMs,
        p_metadata: parsed.metadata,
      });
      if (error) throw new Error(error.message || "telemetry RPC failed");
      eventCollector?.telemetryReported(serverId, {
        tps: parsed.tps,
        mspt: parsed.mspt,
        playerCount: parsed.playerCount,
        entityCount: parsed.entityCount,
        heapUsedMb: parsed.heapUsedMb,
        uptimeMs,
      });
    } catch (err) {
      console.error(`[telemetry] Error reporting for ${serverId}:`, err.message);
      eventCollector?.networkError(`telemetry:${serverId}`, err.message);
    }
  }
}

/**
 * Parse Minecraft server console output for metrics.
 * Looks for common patterns in log lines.
 */
export function parseMinecraftMetrics(lines) {
  const metrics = {
    tps: 20.0,
    mspt: 0,
    playerCount: 0,
    entityCount: 0,
    chunkCount: 0,
    threadCount: 0,
    heapUsedMb: 0,
    heapMaxMb: 0,
    gcCount: 0,
    gcTimeMs: 0,
    metadata: {},
  };

  for (const line of lines) {
    const content = line.content || "";

    // TPS: "TPS: 20.0" or " ticks per second"
    const tpsMatch = content.match(/TPS[:\s]+(\d+\.?\d*)/i);
    if (tpsMatch) {
      metrics.tps = parseFloat(tpsMatch[1]);
    }

    // MSPT: "avg tick time: 5.23ms"
    const msptMatch = content.match(/(?:avg\s+)?tick\s+time[:\s]+(\d+\.?\d*)\s*ms/i);
    if (msptMatch) {
      metrics.mspt = parseFloat(msptMatch[1]);
    }

    // Player count: "[Server] Player connected: ..." or "/list"
    const playerMatch = content.match(/(\d+)\s+of\s+a\s+max\s+(\d+)\s+players/i);
    if (playerMatch) {
      metrics.playerCount = parseInt(playerMatch[1], 10);
      metrics.metadata.max_players = parseInt(playerMatch[2], 10);
    }

    // Entity count: "Entity count: 1234"
    const entityMatch = content.match(/entity\s+count[:\s]+(\d+)/i);
    if (entityMatch) {
      metrics.entityCount = parseInt(entityMatch[1], 10);
    }

    // Chunk count: "Loaded chunks: 5678"
    const chunkMatch = content.match(/(?:loaded\s+)?chunks?[:\s]+(\d+)/i);
    if (chunkMatch) {
      metrics.chunkCount = parseInt(chunkMatch[1], 10);
    }

    // Java memory: "[GC (Allocation Failure) 456M->123M(1024M), 0.123 secs]"
    const gcMatch = content.match(
      /\[GC.*?(\d+)[MG]->(\d+)[MG]\((\d+)[MG]\)/i
    );
    if (gcMatch) {
      metrics.heapUsedMb = parseInt(gcMatch[2], 10);
      metrics.heapMaxMb = parseInt(gcMatch[3], 10);
      metrics.gcCount++;
    }

    // Memory from /gc or /memory command output
    const memMatch = content.match(
      /heap\s*:?\s*(\d+)\s*MB\s*(?:used|\/)\s*(\d+)\s*MB/i
    );
    if (memMatch) {
      metrics.heapUsedMb = parseInt(memMatch[1], 10);
      metrics.heapMaxMb = parseInt(memMatch[2], 10);
    }
  }

  return metrics;
}

/**
 * Start the console + telemetry flush loop.
 */
export function startFlushLoop(supabase, nodeInfo, config, manager, isRunning, eventCollector = null) {
  const consoleInterval = (config.operationPollIntervalSec || 5) * 1000;
  const telemetryInterval = (config.telemetryIntervalSec || 60) * 1000;

  // Console flush loop (every 5s)
  const consoleLoop = async () => {
    while (isRunning()) {
      try {
        await flushConsoleLines(supabase, nodeInfo, config, manager, eventCollector);
      } catch (err) {
        console.error("[flush] Console error:", err.message);
      }
      await sleep(consoleInterval);
    }
  };

  // Telemetry loop (every 60s)
  const telemetryLoop = async () => {
    while (isRunning()) {
      try {
        await collectAndReportTelemetry(supabase, nodeInfo, config, manager, eventCollector);
      } catch (err) {
        console.error("[flush] Telemetry error:", err.message);
      }
      await sleep(telemetryInterval);
    }
  };

  consoleLoop();
  telemetryLoop();
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
