// Retry helper with exponential backoff.
// Wraps any async function with configurable retry logic.

/**
 * Retry an async function with exponential backoff.
 * @param {Function} fn - The async function to retry
 * @param {Object} options - Retry options
 * @param {number} options.maxRetries - Maximum retry attempts (default: 3)
 * @param {number} options.baseDelayMs - Base delay in milliseconds (default: 1000)
 * @param {number} options.maxDelayMs - Maximum delay cap (default: 30000)
 * @param {Function} options.shouldRetry - Custom retry predicate (default: always retry)
 * @param {string} options.label - Label for logging
 */
export async function retry(fn, options = {}) {
  const {
    maxRetries = 3,
    baseDelayMs = 1000,
    maxDelayMs = 30000,
    shouldRetry = () => true,
    label = "operation",
  } = options;

  let lastError;
  for (let attempt = 0; attempt <= maxRetries; attempt++) {
    try {
      return await fn();
    } catch (err) {
      lastError = err;

      if (attempt === maxRetries || !shouldRetry(err)) {
        throw err;
      }

      // Exponential backoff with jitter
      const delay = Math.min(
        baseDelayMs * Math.pow(2, attempt) + Math.random() * baseDelayMs,
        maxDelayMs
      );

      console.warn(
        `[retry] ${label} failed (attempt ${attempt + 1}/${maxRetries + 1}): ${err.message}. Retrying in ${Math.round(delay)}ms...`
      );

      await new Promise((resolve) => setTimeout(resolve, delay));
    }
  }

  throw lastError;
}

/**
 * Retry a Supabase RPC call.
 */
export async function retryRpc(supabase, functionName, args = {}, options = {}) {
  return retry(
    () => supabase.rpc(functionName, args),
    {
      label: `rpc:${functionName}`,
      shouldRetry: (err) => !err.message?.includes("invalid"),
      ...options,
    }
  );
}

/**
 * Retry a Supabase query.
 */
export async function retryQuery(queryFn, options = {}) {
  return retry(queryFn, {
    label: "query",
    ...options,
  });
}

/**
 * Retry a Supabase insert.
 */
export async function retryInsert(supabase, table, data, options = {}) {
  return retry(
    () => supabase.from(table).insert(data),
    {
      label: `insert:${table}`,
      ...options,
    }
  );
}
