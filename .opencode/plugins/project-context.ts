import { existsSync, mkdirSync, readFileSync, writeFileSync, readdirSync, statSync, unlinkSync, rmSync } from "node:fs"
import { join, relative, resolve, basename } from "node:path"
import { createHash } from "node:crypto"
import { execSync } from "node:child_process"
import { tmpdir } from "node:os"
import type { Plugin } from "@opencode-ai/plugin"
import { tool } from "@opencode-ai/plugin"

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

type AIProvider = "openai-compatible" | "ollama" | "anthropic"

type AIConfig = {
  enabled: boolean
  provider: AIProvider
  baseUrl: string                 // overrides per-provider default
  apiKey: string                   // env interpolation ${VAR}
  model: string                     // tani model np. gpt-4o-mini
  maxTokens: number
  temperature: number
  timeoutMs: number
  fallbackChain: string[]          // kolejne modele do spróbowania
  minIntervalMs: number             // min. odstęp między automatycznymi wywołaniami AI (session.idle/compacted); 0 = brak throttle. Komendy na żądanie (/codemem ai triage) ignorują.
}

type Config = {
  enabled: boolean
  maxProjectMemoryTokens: number
  maxSessionHandoffTokens: number
  maxToolResultLines: number
  maxDiffLines: number
  maxSearchMatches: number
  maxArtifactPreviewLines: number
  deduplicateReadResults: boolean
  storeFullArtifacts: boolean
  // Additions
  persistentDedupCache: boolean
  maxDedupCacheEntries: number
  maxTestHistoryEntries: number
  // Regression detection
  regressionTrackHead: boolean
  regressionSafeRevertOnly: boolean
  // Auto-extracted facts
  autoExtractFacts: boolean
  autoExtractOnEvents: string[]   // e.g. ["session.idle","session.compacted"]
  factsAutoGlobDepth: number
  // Context compaction
  compactMode: "auto" | "suggest" | "confirm" | "off"
  maxContextTokens: number        // 0 = autodetect from Model.limit.context (fallback 200000)
  compactThreshold: number        // percent (0-100) of limit; default 80
  compactReservedTokens: number   // buffer left for compaction (mirrors compaction.reserved)
  // AI (Opcja A — własny tani model, niezależny od modelu kodującego)
  ai: AIConfig
}

const DEFAULT_CONFIG: Config = {
  enabled: true,
  maxProjectMemoryTokens: 1500,
  maxSessionHandoffTokens: 1000,
  maxToolResultLines: 100,
  maxDiffLines: 120,
  maxSearchMatches: 40,
  maxArtifactPreviewLines: 80,
  deduplicateReadResults: true,
  storeFullArtifacts: true,
  // Additions
  persistentDedupCache: true,
  maxDedupCacheEntries: 500,
  maxTestHistoryEntries: 50,
  regressionTrackHead: true,
  regressionSafeRevertOnly: true,
  // Auto-extracted facts
  autoExtractFacts: true,
  autoExtractOnEvents: ["session.idle", "session.compacted"],
  factsAutoGlobDepth: 3,
  // Context compaction
  compactMode: "suggest",
  maxContextTokens: 0,
  compactThreshold: 80,
  compactReservedTokens: 10000,
  // AI — domyślnie wyłączone. Włącz w opencode.json plugin options.
  ai: {
    enabled: false,
    provider: "openai-compatible",
    baseUrl: "",
    apiKey: "",
    model: "gpt-4o-mini",
    maxTokens: 800,
    temperature: 0,
    timeoutMs: 30000,
    fallbackChain: [],
    minIntervalMs: 600000,
  },
}

type SeenContext = {
  filePath: string
  contentHash: string
  lineStart?: number
  lineEnd?: number
  deliveredAt: string
  source: "read" | "grep" | "diff" | "lsp" | "command"
}

type ActiveSession = {
  schemaVersion: 1
  sessionId: string
  updatedAt: string
  goal: string
  currentStatus: string
  modifiedFiles: string[]
  decisions: string[]
  commands: Record<string, string>
  testStatus?: {
    lastCommand: string
    exitCode: number
    summary: string
  }
  blockers: string[]
  lspErrors?: string[]
}

type Metrics = {
  sessionId: string
  toolCalls: number
  rawChars: number
  deliveredChars: number
  deduplicatedReads: number
  dedupSavedChars: number
  estimatedReductionPercent: number
  estimatedSavedChars: number
  estimatedSavedTokens: number
  artifactsCreated: number
  artifactBytes: number
  // --- TUI live stats (filled in flushMetrics) ---
  contextTokens: number
  contextLimit: number
  compactThresholdPct: number
  compactMode: string
  headSha: string
  dirtyFiles: number
  diskBytes: number
  diskLimitBytes: number
  artifactsBytes: number
  artifactsList: { id: string; bytes: number }[]  // dla TUI (unika readdirSync+statSync co 3s)
  cacheBytes: number
  handoffAgeMin: number
  modifiedCount: number
  decisionsCount: number
  blockersCount: number
  dedupCacheCount: number
  dedupCacheMax: number
  testHistoryCount: number
  testHistoryMax: number
  lspErrorsCount: number
  lastGoodHead: string
  revertsCount: number
  factsTokens: number
  factsMaxTokens: number
  // --- AI (Opcja A) live stats ---
  aiEnabled: boolean
  aiProvider: string
  aiModel: string
  aiCalls: number
  aiSuccesses: number
  aiFailures: number
  aiLastCallMs: number
  aiLastError: string
  aiHealthState: "active" | "offline" | "unknown"
  aiBusy: boolean
  aiBusyLabel: string
  aiBusySince: number
  // --- Dynamiczny timeout ---
  aiConfigTimeoutMs: number
  aiMaxObservedMs: number
  aiLastDurationMs: number
  aiTimeoutWarn: boolean
  aiTimeoutExtended: boolean
}

type TestRun = {
  timestamp: string
  command: string
  exitCode: number
  summary: string
  failed: string[]
  sessionId: string
  head: string  // git SHA w momencie uruchomienia (do korelacji regresji)
}

type SessionTrace = {
  sessionId: string
  buildTestCommands: Record<string, number>   // command -> invocation count
  editedFiles: Record<string, number>          // file -> edit count across sessions
  blockers: string[]                          // repeated blockers seen
  startedAt: string
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const SECRET_PATTERNS: RegExp[] = [
  /API_KEY\s*=\s*[^\s]+/gi,
  /SECRET\s*=\s*[^\s]+/gi,
  /PASSWORD\s*=\s*[^\s]+/gi,
  /TOKEN\s*=\s*[^\s]+/gi,
  /Bearer\s+[A-Za-z0-9\-\._~+\/=]+/g,
  /Authorization:\s*Bearer\s+[^\s]+/gi,
  /-----BEGIN [A-Z ]+PRIVATE KEY-----[\s\S]*?-----END [A-Z ]+PRIVATE KEY-----/g,
  /AKIA[0-9A-Z]{16}/g, // AWS access key id
  /ghp_[A-Za-z0-9]{36}/g, // GitHub PAT
  /gho_[A-Za-z0-9]{36}/g,
  /sk-[A-Za-z0-9]{20,}/g,
]

const SENSITIVE_PATH_PATTERNS = [
  /\.env(\.|$)/i,
  /id_rsa/i,
  /\.pem$/i,
  /\.p12$/i,
  /\.kdbx$/i,
  /credentials/i,
  /secrets/i,
]

const MAX_ARTIFACT_DIR_MB = 200
const ARTIFACT_TTL_MS = 1000 * 60 * 60 * 24 * 7 // 7 days

// ---------------------------------------------------------------------------
// Plugin state (per plugin instance / per worktree)
// ---------------------------------------------------------------------------

let cfg: Config = { ...DEFAULT_CONFIG }
let memoryDir = ""
let worktreePath = ""
let projectRoot = ""
let seen: Map<string, SeenContext> = new Map()

function dedupKey(s: Pick<SeenContext, "filePath" | "contentHash" | "lineStart" | "lineEnd">): string {
  return `${s.filePath}::${s.contentHash}::${s.lineStart ?? 0}-${s.lineEnd ?? "end"}`
}
let metrics: Metrics = {
  sessionId: "",
  toolCalls: 0,
  rawChars: 0,
  deliveredChars: 0,
  deduplicatedReads: 0,
  dedupSavedChars: 0,
  estimatedReductionPercent: 0,
  estimatedSavedChars: 0,
  estimatedSavedTokens: 0,
  artifactsCreated: 0,
  artifactBytes: 0,
  contextTokens: 0,
  contextLimit: 0,
  compactThresholdPct: 80,
  compactMode: "suggest",
  headSha: "",
  dirtyFiles: 0,
  diskBytes: 0,
  diskLimitBytes: MAX_ARTIFACT_DIR_MB * 1024 * 1024,
  artifactsBytes: 0,
  artifactsList: [],
  cacheBytes: 0,
  handoffAgeMin: 0,
  modifiedCount: 0,
  decisionsCount: 0,
  blockersCount: 0,
  dedupCacheCount: 0,
  dedupCacheMax: 500,
  testHistoryCount: 0,
  testHistoryMax: 50,
  lspErrorsCount: 0,
  lastGoodHead: "",
  revertsCount: 0,
  factsTokens: 0,
  factsMaxTokens: 1500,
  aiEnabled: false,
  aiProvider: "",
  aiModel: "",
  aiCalls: 0,
  aiSuccesses: 0,
  aiFailures: 0,
  aiLastCallMs: 0,
  aiLastError: "",
  aiHealthState: "unknown",
  aiBusy: false,
  aiBusyLabel: "",
  aiBusySince: 0,
  aiConfigTimeoutMs: 0,
  aiMaxObservedMs: 0,
  aiLastDurationMs: 0,
  aiTimeoutWarn: false,
  aiTimeoutExtended: false,
}
let lastInjectedContext = ""
let pendingSystemContext: string = ""                 // blok PROJECT MEMORY do wstrzykiwania w system prompt (experimental.chat.system.transform)
let lastSessionId = ""

// --- Additions: persistent dedup cache, test history, session trace ----------
let testHistory: TestRun[] = []
let sessionTrace: SessionTrace = {
  sessionId: "",
  buildTestCommands: {},
  editedFiles: {},
  blockers: [],
  startedAt: "",
}

// --- Context compaction state -------------------------------------------------
let lastContextTokens: number = 0          // bieżący rozmiar kontekstu (AssistantMessage.tokens.input)
let modelContextLimit: number = 0          // 0 = nie ustalono; autodetekcja z Model.limit.context
let compactSuggestionShown: boolean = false // czy już pokazano sugestię dla obecnego przekroczenia
let lastAssistantModel: string = ""        // "provider/model" do autodetekcji limitu
let currentSessionId: string = ""           // bieżąca sesja (do client.session.messages)
let pluginClient: any = null                 // ref client API z closure pluginu (do compact-now, ai triage)
let lastUserCommandTs: number = 0          // timestamp ostatniej komendy /codemem — pomija throttle auto-AI w idle

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function failOpen(fn: () => void, label: string) {
  try {
    fn()
  } catch (e: any) {
    try {
      const logPath = join(memoryDir || process.cwd(), "plugin-errors.log")
      rotateLogIfNeeded(logPath)
      writeFileSync(logPath, `[${new Date().toISOString()}] ${label}: ${e?.message ?? e}\n`, { flag: "a" })
    } catch {
      // swallow
    }
  }
}

function failOpenReturn<T>(fn: () => T, fallback: T, label: string): T {
  try {
    return fn()
  } catch (e: any) {
    try {
      const logPath = join(memoryDir || process.cwd(), "plugin-errors.log")
      rotateLogIfNeeded(logPath)
      writeFileSync(logPath, `[${new Date().toISOString()}] ${label}: ${e?.message ?? e}\n`, { flag: "a" })
    } catch {
      // swallow
    }
    return fallback
  }
}

const FAIL_OPEN_ASYNC_TIMEOUT_MS = 35000

async function failOpenAsync(fn: () => Promise<void> | void, label: string) {
  try {
    const task = fn()
    if (task && typeof (task as Promise<void>).then === "function") {
      // Ochrona przed zawieszonymi promisami (np. fetch bez odpowiedzi, session.prompt
      // wiszący na SDK). Bez tego opencode czeka w nieskończoność i ESC nie przerywa.
      await Promise.race([
        task as Promise<void>,
        new Promise<void>((_, reject) =>
          setTimeout(() => reject(new Error(`timeout ${FAIL_OPEN_ASYNC_TIMEOUT_MS}ms`)), FAIL_OPEN_ASYNC_TIMEOUT_MS)
        ),
      ])
    }
  } catch (e: any) {
    try {
      const logPath = join(memoryDir || process.cwd(), "plugin-errors.log")
      rotateLogIfNeeded(logPath)
      writeFileSync(logPath, `[${new Date().toISOString()}] ${label}: ${e?.message ?? e}\n`, { flag: "a" })
    } catch {
      // swallow
    }
  }
}

function ensureDir(dir: string) {
  if (!existsSync(dir)) mkdirSync(dir, { recursive: true })
}

function readJson<T>(path: string): T | null {
  if (!existsSync(path)) return null
  try {
    return JSON.parse(readFileSync(path, "utf8")) as T
  } catch {
    return null
  }
}

function writeJson(path: string, data: unknown) {
  ensureDir(join(path, ".."))
  writeFileSync(path, JSON.stringify(data, null, 2), "utf8")
}

function readText(path: string): string {
  if (!existsSync(path)) return ""
  return readFileSync(path, "utf8")
}

function estimateTokens(text: string): number {
  return Math.ceil(text.length / 4)
}

// Truncate by byte budget without splitting multi-byte UTF-8 characters
// ( Polish diacritics like ł, ą, ż are multi-byte; naive .slice() can
//   produce a trailing replacement character / corrupted glyph ).
function truncateByBytes(text: string, maxBytes: number): string {
  const buf = Buffer.from(text, "utf8")
  if (buf.length <= maxBytes) return text
  return buf.subarray(0, maxBytes).toString("utf8")
}

function truncateLines(text: string, maxLines: number): string {
  const lines = text.split("\n")
  if (lines.length <= maxLines) return text
  return lines.slice(0, maxLines).join("\n") + `\n... [truncated ${lines.length - maxLines} lines]`
}

function maskSecrets(text: string): string {
  let out = text
  for (const p of SECRET_PATTERNS) {
    out = out.replace(p, (match) => {
      // keep key name, mask value
      if (match.includes("=")) {
        const idx = match.indexOf("=")
        return match.slice(0, idx + 1) + "...[REDACTED]"
      }
      if (match.includes(":")) {
        const idx = match.indexOf(":")
        return match.slice(0, idx + 1) + " ...[REDACTED]"
      }
      if (match.startsWith("Bearer ")) return "Bearer ...[REDACTED]"
      return "...[REDACTED]"
    })
  }
  return out
}

function hashContent(content: string): string {
  return createHash("sha256").update(content).digest("hex")
}

function shortHash(content: string): string {
  return createHash("sha256").update(content).digest("hex").slice(0, 6)
}

function isSensitivePath(filePath: string): boolean {
  const norm = filePath.replace(/\\/g, "/").toLowerCase()
  return SENSITIVE_PATH_PATTERNS.some((p) => p.test(norm))
}

// ---------------------------------------------------------------------------
// AI module (Opcja A — własny tani model, niezależny od modelu kodującego)
// ---------------------------------------------------------------------------
// aiComplete() woła HTTP endpoint taniego modelu konfigurowanego w plugin options.
// NIGDY nie rzuca — zwraca null przy błędzie; wywołujący ma fallback deterministyczny.
// Fallbacki (warstwowo): brak konfiga → null; network/HTTP error → retry+chain → null;
// zła odpowiedź → retry temp:0 → null.

type AIStatus = {
  enabled: boolean
  provider: string
  model: string
  lastCallMs: number
  lastError: string
  calls: number
  successes: number
  failures: number
  healthState: "active" | "offline" | "unknown"
  busy: boolean
  busyLabel: string
  busySince: number
  // --- Dynamiczny timeout ---
  configTimeoutMs: number           // pierwotny timeout z configa (do porównań w TUI)
  maxObservedMs: number            // najdłuższy zarejestrowany czas promptu (persistent)
  lastDurationMs: number           // czas ostatniego promptu
  timeoutWarn: boolean             // true gdy ostatni prompt ≥80% limitu (zbliża się)
  timeoutExtended: boolean         // true gdy ostatni prompt przekroczył pierwotny limit (ale zmieścił się w 1.5×)
}

let aiStatus: AIStatus = {
  enabled: false,
  provider: "",
  model: "",
  lastCallMs: 0,
  lastError: "",
  calls: 0,
  successes: 0,
  failures: 0,
  healthState: "unknown",
  busy: false,
  busyLabel: "",
  busySince: 0,
  configTimeoutMs: 0,
  maxObservedMs: 0,
  lastDurationMs: 0,
  timeoutWarn: false,
  timeoutExtended: false,
}

// Circuit breaker: po N kolejnych awariach w oknie czasowym AI jest wyłączane,
// żeby nie tworzyć pętli błędów (np. endpoint 500 w nieskończoność).
const AI_CB_THRESHOLD = 5          // po tylu kolejnych awariach wyłączamy
const AI_CB_WINDOW_MS = 60_000      // okno liczenia awarii (1 min)
let aiCbFailuresSince: number = 0    // licznik kolejnych awarii
let aiCbFirstFailureAt: number = 0   // timestamp pierwszej awarii w oknie
let aiCbTrippedUntil: number = 0     // do kiedy breaker jest otwarty (0 = zamknięty)

function aiCbShouldSkip(): boolean {
  if (aiCbTrippedUntil && Date.now() < aiCbTrippedUntil) return true
  if (aiCbTrippedUntil && Date.now() >= aiCbTrippedUntil) {
    // reset po cooldownie — spróbuj ponownie
    aiCbTrippedUntil = 0
    aiCbFailuresSince = 0
  }
  return false
}

function aiCbRecordFailure() {
  const now = Date.now()
  if (!aiCbFirstFailureAt || (now - aiCbFirstFailureAt) > AI_CB_WINDOW_MS) {
    aiCbFirstFailureAt = now
    aiCbFailuresSince = 1
  } else {
    aiCbFailuresSince += 1
  }
  if (aiCbFailuresSince >= AI_CB_THRESHOLD) {
    aiCbTrippedUntil = now + AI_CB_WINDOW_MS
    aiLogError(`AI circuit breaker TRIPPED: ${aiCbFailuresSince} failures in window, pausing AI for ${AI_CB_WINDOW_MS}ms`)
  }
}

function aiCbRecordSuccess() {
  aiCbFailuresSince = 0
  aiCbFirstFailureAt = 0
  aiCbTrippedUntil = 0
}

// --- AI auto-run throttle -----------------------------------------------------
// #3: ogranicza częstotliwość AUTOMATYCZNYCH wywołań AI (aiSummarizeSession
// na session.idle/compacted). Komendy na żądanie (/codemem ai triage),
// health check na session.created oraz aiExtractHumanFacts NIE są throttlowane.
// aiExtractHumanFacts używa własnego change-detection (hash plików źródłowych).
// Stan trzyma timestamp ostatniego auto-wywołania w pliku cache.
function aiThrottlePath(): string {
  return join(memoryDir, "cache", "ai-throttle.json")
}

function aiAutoThrottleMs(): number {
  const c = cfg?.ai
  if (!c || typeof c.minIntervalMs !== "number" || c.minIntervalMs < 0) return 600000
  return c.minIntervalMs
}

// Zwraca true jeśli auto-wywołanie AI powinno być pominięte (zbyt wkrótce od ostatniego).
// Pomija throttle gdy idle/compacted nastąpiło w wyniku komendy użytkownika (okno 30 s).
function aiAutoThrottled(): boolean {
  const minInterval = aiAutoThrottleMs()
  if (minInterval <= 0) return false        // 0 = throttle wyłączony
  if (lastUserCommandTs > 0 && (Date.now() - lastUserCommandTs) < 30000) return false
  const data = readJson<{ lastAutoRunTs?: number } | null>(aiThrottlePath())
  const last = data?.lastAutoRunTs ?? 0
  return (Date.now() - last) < minInterval
}

// Zapisuje timestamp bieżącego auto-wywołania (wołać TYLKO przy rzeczywistym auto-wywołaniu).
function aiAutoMarkRun() {
  try { writeJson(aiThrottlePath(), { lastAutoRunTs: Date.now() }) } catch { /* swallow */ }
}

// --- Dynamiczny timeout: śledzenie najdłuższego czasu promptu -----------------
// maxObservedMs jest persistentne (przeżywa restarty sesji), bo celem jest
// adaptacja timeoutu do realnego zachowania lokalnego modelu w czasie.
function aiMaxObservedPath(): string {
  return join(memoryDir, "cache", "ai-max-observed.json")
}

function aiLoadMaxObserved(): number {
  const data = readJson<{ maxObservedMs?: number } | null>(aiMaxObservedPath())
  return (data && typeof data.maxObservedMs === "number") ? data.maxObservedMs : 0
}

function aiSaveMaxObserved(ms: number) {
  try { writeJson(aiMaxObservedPath(), { maxObservedMs: ms }) } catch { /* swallow */ }
}

// Efektywny twardy timeout: pierwotny timeoutMs × 1.5 (dopuszczamy o 50% dłuższy czas).
function aiEffectiveTimeoutMs(c: AIConfig): number {
  const base = c.timeoutMs > 0 ? c.timeoutMs : 30000
  return Math.round(base * 1.5)
}

// Próg ostrzeżenia "zbliża się do limitu" — 80% pierwotnego timeoutMs.
function aiTimeoutWarnThresholdMs(c: AIConfig): number {
  const base = c.timeoutMs > 0 ? c.timeoutMs : 30000
  return Math.round(base * 0.8)
}

// --- Timeout override (z komendy /codemem ai auto-timeout) --------------------
// Ustawia pierwotny timeoutMs na wartość większą o 30% od najdłuższego dotąd
// zarejestrowanego promptu. Przeżywa restarty (zapisane w cache).
function aiTimeoutOverridePath(): string {
  return join(memoryDir, "cache", "ai-timeout-override.json")
}

function aiLoadTimeoutOverride(): number {
  const data = readJson<{ timeoutMs?: number } | null>(aiTimeoutOverridePath())
  return (data && typeof data.timeoutMs === "number" && data.timeoutMs > 0) ? data.timeoutMs : 0
}

function aiSaveTimeoutOverride(ms: number) {
  try { writeJson(aiTimeoutOverridePath(), { timeoutMs: ms }) } catch { /* swallow */ }
}

function aiClearTimeoutOverride() {
  try { rmSync(aiTimeoutOverridePath(), { force: true }) } catch { /* swallow */ }
}

// /codemem ai auto-timeout: ustaw timeoutMs = maxObservedMs * 1.3 (min. 30 s).
// Zwraca komunikat potwierdzający. Gdy brak obserwacji, sugeruje retry później.
function aiAutoTimeoutCommand(): string {
  const observed = aiStatus.maxObservedMs || aiLoadMaxObserved()
  if (observed <= 0) {
    return [
      "AI auto-timeout: brak zarejestrowanych czasów promptów.",
      "Wywołaj komendę po co najmniej jednym udanym prompcie AI.",
    ].join("\n")
  }
  const newTimeout = Math.max(30000, Math.round(observed * 1.3))
  cfg.ai.timeoutMs = newTimeout
  aiSaveTimeoutOverride(newTimeout)
  aiStatus.configTimeoutMs = newTimeout
  flushMetrics()
  return [
    `AI auto-timeout: ustawiono timeoutMs = ${newTimeout}ms`,
    `  (najdłuższy prompt: ${observed}ms × 1.3 + zaokrąglenie)`,
    `  Twardy limit (1.5×): ${Math.round(newTimeout * 1.5)}ms`,
    `  Override zapisany w cache/ai-timeout-override.json.`,
  ].join("\n")
}

const LOG_ROTATE_MAX_BYTES = 1_000_000   // 1 MB — przy tnij do ostatnich 200 linii

function rotateLogIfNeeded(logPath: string) {
  try {
    const st = statSync(logPath)
    if (st.size > LOG_ROTATE_MAX_BYTES) {
      const raw = readFileSync(logPath, "utf8")
      const lines = raw.split("\n")
      const kept = lines.slice(-200).join("\n")
      writeFileSync(logPath, kept, "utf8")
    }
  } catch { /* swallow */ }
}

function aiLogError(msg: string) {
  aiStatus.lastError = msg
  try {
    const logPath = join(memoryDir || process.cwd(), "plugin-ai.log")
    rotateLogIfNeeded(logPath)
    writeFileSync(logPath, `[${new Date().toISOString()}] ${msg}\n`, { flag: "a" })
  } catch { /* swallow */ }
}

// Parsuje "providerID/modelID" — dzieli tylko na pierwszym slashu, by obsłużyć
// modelID zawierające slashe (np. "openrouter/cohere/north-mini-code:free").
// Zwraca [providerID, modelID] lub ["", ""] gdy niepoprawny.
function parseModelKey(modelKey: string): [string, string] {
  if (!modelKey || typeof modelKey !== "string") return ["", ""]
  const slashIdx = modelKey.indexOf("/")
  if (slashIdx <= 0 || slashIdx >= modelKey.length - 1) return ["", ""]
  return [modelKey.slice(0, slashIdx), modelKey.slice(slashIdx + 1)]
}

// Bezpieczna serializacja błędu/obiektu do stringa (nigdy nie zwraca "[object Object]").
function safeErrorString(e: any): string {
  if (e == null) return "null"
  if (typeof e === "string") return e
  if (e?.message && typeof e.message === "string") return e.message
  if (e?.error && typeof e.error === "string") return e.error
  try { return JSON.stringify(e) } catch { return String(e) }
}

// Interpolacja ${ENV_VAR} w wartościach konfiga (np. apiKey: "${OPENAI_API_KEY}").
// Obsługuje też literał bez zmiennej środowiskowej: ${literal} lub ${ns:literal}
// — zwraca wtedy wartość dosłowną (bez ${ i }), by móc wpisać apiKey inline.
function interpolateEnv(value: string): string {
  if (!value || typeof value !== "string") return value
  return value.replace(/\$\{([^}]+)\}/g, (_, inner: string) => {
    const name = inner.trim()
    // Jeśli nazwa jest poprawnym identyfikatorem zmiennej środowiskowej, zinterpoluj.
    if (/^[A-Z_][A-Z0-9_]*$/.test(name)) {
      return process.env[name] ?? ""
    }
    // Inaczej: zwróć wartość dosłowną (obsługa ${sk-lm-xxx:yyy} itp.).
    return name
  })
}

function aiEffectiveConfig(): AIConfig | null {
  const c = cfg.ai
  if (!c || !c.enabled) return null
  const apiKey = interpolateEnv(c.apiKey).trim()
  // ollama lokalnie nie wymaga apiKey
  if (c.provider !== "ollama" && !apiKey) return null
  return {
    ...c,
    apiKey,
    baseUrl: c.baseUrl || aiDefaultBaseUrl(c.provider),
  }
}

function aiDefaultBaseUrl(provider: AIProvider): string {
  switch (provider) {
    case "ollama": return "http://localhost:11434"
    case "anthropic": return "https://api.anthropic.com/v1"
    case "openai-compatible":
    default: return "https://api.openai.com/v1"
  }
}

// Buduje body zapytania per provider
function aiBuildBody(c: AIConfig, system: string, prompt: string, jsonMode: boolean): any {
  if (c.provider === "anthropic") {
    return {
      model: c.model,
      max_tokens: c.maxTokens,
      temperature: c.temperature,
      system,
      messages: [{ role: "user", content: prompt }],
    }
  }
  // openai-compatible + ollama (oba używają /chat/completions)
  const body: any = {
    model: c.model,
    max_tokens: c.maxTokens,
    temperature: c.temperature,
    messages: [
      ...(system ? [{ role: "system", content: system }] : []),
      { role: "user", content: prompt }],
  }
  if (jsonMode && c.provider !== "ollama") body.response_format = { type: "json_object" }
  // Wyłącz reasoning (thinking) dla modeli Qwen3.5/Llama4 — LM Studio honoruje
  // reasoning_effort:"none"; chat_template_kwargs jest fallbackiem (ignorowane przez LM Studio).
  if (c.provider === "openai-compatible") {
    body.reasoning_effort = "none"
    body.chat_template_kwargs = { enable_thinking: false }
  }
  return body
}

function aiBuildHeaders(c: AIConfig): Record<string, string> {
  if (c.provider === "anthropic") {
    return {
      "content-type": "application/json",
      "x-api-key": c.apiKey,
      "anthropic-version": "2023-06-01",
    }
  }
  return {
    "content-type": "application/json",
    authorization: `Bearer ${c.apiKey}`,
  }
}

function aiUrl(c: AIConfig): string {
  let base = c.baseUrl.replace(/\/$/, "")
  if (c.provider === "anthropic") return `${base}/messages`
  // openai-compatible: auto-dopisz /v1 jeśli brakuje (LM Studio, Ollama-compat).
  // Ollama natywny ma własny endpoint /api/chat, ale fallback przez /chat/completions.
  if (c.provider === "openai-compatible" && !/\/v\d+$/.test(base)) {
    base += "/v1"
  }
  return `${base}/chat/completions`
}

// Ekstrakcja tekstu z odpowiedzi niezależnie od provider'a
function aiExtractText(c: AIConfig, body: any): string {
  if (c.provider === "anthropic") {
    const content = body?.content
    if (Array.isArray(content)) {
      return content.map((p: any) => p?.text ?? "").join("")
    }
    return ""
  }
  // openai-compatible / ollama
  const choice = body?.choices?.[0]
  const msg = choice?.message
  // Preferuj content; fallback na reasoning_content gdy model (np. Qwen3.5) wypluwł
  // tylko thinking — LM Studio czasem zwraca content:"" i reasoning_content z treścią.
  // W trybie reasoning_effort:none to nie powinno się zdarzać, ale zostawiamy awaryjnie.
  const content = msg?.content ?? ""
  if (content.trim()) return content
  const reasoning = msg?.reasoning_content ?? ""
  return reasoning
}

// Pojedyncze wołanie HTTP z timeoutem. Zwraca tekst lub rzuca.
async function aiCallOnce(c: AIConfig, system: string, prompt: string, jsonMode: boolean): Promise<string> {
  const url = aiUrl(c)
  const headers = aiBuildHeaders(c)
  const body = JSON.stringify(aiBuildBody(c, system, prompt, jsonMode))
  const ctrl = new AbortController()
  // Dynamiczny timeout: dopuszczamy o 50% dłuższy czas niż pierwotny timeoutMs.
  // Gdy pierwotny limit minie, dostajemy timeoutExtended flag (w aiComplete),
  // a twardy cutoff następuje dopiero przy 1.5×.
  const hardTimeout = aiEffectiveTimeoutMs(c)
  const timer = setTimeout(() => ctrl.abort(), hardTimeout)
  try {
    const res = await fetch(url, { method: "POST", headers, body, signal: ctrl.signal })
    if (!res.ok) {
      const text = await res.text().catch(() => "")
      throw new Error(`HTTP ${res.status}: ${text.slice(0, 200)}`)
    }
    const data = await res.json() as any
    const text = aiExtractText(c, data)
    if (!text || !text.trim()) throw new Error("empty response")
    return text
  } finally {
    clearTimeout(timer)
  }
}

// Spróbuj sparsować JSON bez rzucania; zwraca null przy błędzie
function tryParseJson(text: string): any | null {
  try {
    // wytnij blok ```json ... ``` jeśli występuje
    const m = text.match(/```(?:json)?\s*([\s\S]*?)```/i)
    const raw = m ? m[1] : text
    return JSON.parse(raw.trim())
  } catch {
    return null
  }
}

// Główna funkcja AI. Zwraca null przy każdym błędzie (nigdy nie rzuca).
// Wywołujący ma fallback deterministyczny.
async function aiComplete(opts: {
  system?: string
  prompt: string
  jsonMode?: boolean
  label?: string
}): Promise<string | null> {
  // Circuit breaker: jeśli AI awariuje w pętli, przestań próbować — pozwól
  // deterministycznym fallbackom działać i nie blokuj runtime'u opencode.
  if (aiCbShouldSkip()) return null
  // Ścieżka HTTP: własny fetch z ai config.
  const c = aiEffectiveConfig()
  if (!c) return null
  aiStatus.enabled = true
  aiStatus.provider = c.provider
  aiStatus.model = c.model

  const models = [c.model, ...c.fallbackChain]
  const system = opts.system ?? ""
  const prompt = opts.prompt
  const jsonMode = opts.jsonMode ?? false

  // Zasygnalizuj w TUI, że model pracuje — flush synchronicznie, by user
  // widział czerwony "working…" zanim jakikolwiek await się rozpocznie.
  aiStatus.busy = true
  aiStatus.busyLabel = opts.label ?? "processing"
  aiStatus.busySince = Date.now()
  aiStatus.configTimeoutMs = c.timeoutMs
  aiStatus.timeoutWarn = false
  aiStatus.timeoutExtended = false
  flushMetrics()

  // Helper: zaktualizuj metryki czasu promptu (wołany po każdej próbie).
  const recordDuration = (ms: number) => {
    aiStatus.lastDurationMs = ms
    const warnThreshold = aiTimeoutWarnThresholdMs(c)
    aiStatus.timeoutWarn = ms >= warnThreshold
    aiStatus.timeoutExtended = ms > c.timeoutMs
    if (ms > aiStatus.maxObservedMs) {
      aiStatus.maxObservedMs = ms
      aiSaveMaxObserved(ms)
    }
  }

  try {
    for (let i = 0; i < models.length; i++) {
      const model = models[i]
      const cc = { ...c, model }
      const t0 = Date.now()
      aiStatus.calls += 1
      try {
        let text = await aiCallOnce(cc, system, prompt, jsonMode)
        // Jeśli JSON mode, zweryfikuj parsowaniem
        if (jsonMode) {
          let parsed = tryParseJson(text)
          if (!parsed) {
            // retry 1× z temp:0
            const cc0 = { ...cc, temperature: 0 }
            aiStatus.calls += 1
            try {
              text = await aiCallOnce(cc0, system, prompt, jsonMode)
              parsed = tryParseJson(text)
            } catch (e: any) {
              aiLogError(`retry temp:0 failed (${model}): ${e?.message ?? e}`)
            }
            if (!parsed) {
              aiStatus.failures += 1
              aiLogError(`bad JSON from ${model}: ${text.slice(0, 200)}`)
              continue // spróbuj kolejny model z fallbackChain
            }
          }
        }
        const duration = Date.now() - t0
        aiStatus.successes += 1
        aiStatus.lastCallMs = duration
        aiStatus.lastError = ""
        recordDuration(duration)
        aiCbRecordSuccess()
        return text
      } catch (e: any) {
        const duration = Date.now() - t0
        aiStatus.failures += 1
        aiStatus.lastCallMs = duration
        recordDuration(duration)
        const msg = e?.name === "AbortError" ? `timeout (${aiEffectiveTimeoutMs(cc)}ms)` : safeErrorString(e)
        aiLogError(`call failed (${model}): ${msg}`)
        aiCbRecordFailure()
        // spróbuj kolejny model z fallbackChain (jeśli błąd sieciowy / HTTP)
        continue
      }
    }
    return null
  } finally {
    aiStatus.busy = false
    aiStatus.busyLabel = ""
    aiStatus.busySince = 0
    flushMetrics()
  }
}

// Krótki ping do modelu AI przy starcie sesji — weryfikuje czy endpoint
// odpowiada i działa. Nadpisuje aiStatus wynikiem: active (sukces) / offline
// (porażka). Stare błędy z poprzedniej sesji są kasowane, by TUI nie pokazywało
// przestarzałego komunikatu na starcie.
async function aiHealthCheck(): Promise<void> {
  const c = aiEffectiveConfig()
  if (!c) return
  const t0 = Date.now()
  aiStatus.busy = true
  aiStatus.busyLabel = "health check"
  aiStatus.busySince = Date.now()
  flushMetrics()
  try {
    await aiCallOnce(c, "", "Reply with the single word: ok", false)
    aiStatus.enabled = true
    aiStatus.provider = c.provider
    aiStatus.model = c.model
    aiStatus.lastCallMs = Date.now() - t0
    aiStatus.lastError = ""
    aiStatus.calls = 0
    aiStatus.successes = 0
    aiStatus.failures = 0
    aiStatus.healthState = "active"
    aiCbRecordSuccess()
  } catch (e: any) {
    aiStatus.enabled = true
    aiStatus.provider = c.provider
    aiStatus.model = c.model
    aiStatus.lastCallMs = Date.now() - t0
    const msg = e?.name === "AbortError" ? `timeout (${c.timeoutMs}ms)` : safeErrorString(e)
    aiStatus.lastError = msg
    aiStatus.calls = 0
    aiStatus.successes = 0
    aiStatus.failures = 1
    aiStatus.healthState = "offline"
    aiLogError(`health check failed: ${msg}`)
    aiCbRecordFailure()
  } finally {
    aiStatus.busy = false
    aiStatus.busyLabel = ""
    aiStatus.busySince = 0
    flushMetrics()
  }
}
function aiStatusText(): string {
  if (!aiStatus.enabled && !aiEffectiveConfig()) {
    return [
      "AI: wyłączone.",
      "Aby włączyć, dodaj w opencode.json plugin options:",
      '  "ai": { "enabled": true, "provider": "openai-compatible", "apiKey": "${OPENAI_API_KEY}", "model": "gpt-4o-mini" }',
      "Inni providerzy: \"ollama\" (lokalny, bez apiKey), \"anthropic\".",
    ].join("\n")
  }
  const lines: string[] = []
  const eff = aiEffectiveConfig()
  const enabled = aiStatus.enabled || !!eff
  lines.push(`AI: ${enabled ? "włączone" : "wyłączone (brak apiKey)"}`)
  lines.push(`Provider: ${eff?.provider ?? aiStatus.provider}`)
  lines.push(`Model: ${eff?.model ?? aiStatus.model}`)
  lines.push(`Wołania: ${aiStatus.calls} (sukces: ${aiStatus.successes}, porażka: ${aiStatus.failures})`)
  lines.push(`Ostatni czas: ${aiStatus.lastCallMs}ms`)
  // Dynamiczny timeout
  const cfgTimeout = eff?.timeoutMs ?? aiStatus.configTimeoutMs ?? 0
  const hardLimit = cfgTimeout > 0 ? Math.round(cfgTimeout * 1.5) : 0
  lines.push(`Timeout: ${cfgTimeout}ms (twardy limit 1.5×: ${hardLimit}ms)`)
  if (aiStatus.maxObservedMs > 0) {
    lines.push(`Najdłuższy prompt: ${aiStatus.maxObservedMs}ms`)
  }
  if (aiStatus.timeoutExtended) {
    lines.push(`⚠ Ostatni prompt przekroczył pierwotny limit (${cfgTimeout}ms) — wymaga wydłużenia.`)
  } else if (aiStatus.timeoutWarn) {
    lines.push(`⚠ Ostatni prompt zbliżył się do limitu (≥80% ${cfgTimeout}ms) — może wymagać wydłużenia.`)
  }
  lines.push(aiStatus.lastError ? `Ostatni błąd: ${aiStatus.lastError}` : "Brak błędów.")
  if (aiStatus.failures > 0) lines.push("Fallback deterministyczny aktywny (AI nie przeszkadza).")
  lines.push("Komenda: /codemem ai auto-timeout — wydłuża timeout do max×1.3")
  return lines.join("\n")
}

// --- AI-enhanced: triage ostatnich failed tests -------------------------------
// Analizuje logi nieudanych testów i proponuje root cause. Fallback: deterministyczna lista.
async function aiTriageFailedTests(): Promise<string> {
  const fails = testHistory.filter((t) => t.exitCode !== 0).slice(-3)
  if (!fails.length) return "Brak nieudanych testów w historii."
  const ctx = fails.map((t) => {
    return `## ${t.command} (exit ${t.exitCode}, ${t.timestamp})\nSummary: ${t.summary}\nFailed: ${t.failed.slice(0, 5).join(", ")}`
  }).join("\n\n")
  const system = "Jesteś inżynierem QA. Analizujesz logi nieudanych testów i wskazujesz prawdopodobny root cause. Odpowiadaj zwięźle po polsku, max 5 punktów."
  const prompt = `Oto ostatnie nieudane testy:\n\n${ctx}\n\nWymień prawdopodobne root causes (max 5, krótko):`
  const out = await aiComplete({ system, prompt, label: "triage failed tests" })
  if (!out) {
    // fallback deterministyczny
    return ["AI triage niedostępne. Ostatnie nieudane testy:", "", ctx].join("\n")
  }
  return out
}

// --- AI-enhanced: podsumowanie sesji do handoffa -------------------------------
// Zwraca krótki status (1-2 zdania) na bazie edytowanych plików + testów. Fallback: puste.
async function aiSummarizeSession(edits: string[], testStatus?: { command: string; exitCode: number; summary: string }, extra?: { decisions?: string[]; blockers?: string[] }): Promise<string> {
  const bits: string[] = []
  if (edits.length) bits.push(`Edytowane pliki: ${edits.slice(0, 10).join(", ")}`)
  if (testStatus) bits.push(`Test: ${testStatus.command} (exit ${testStatus.exitCode}): ${testStatus.summary}`)
  if (extra?.decisions?.length) bits.push(`Decyzje: ${extra.decisions.join(" | ")}`)
  if (extra?.blockers?.length) bits.push(`Blokery: ${extra.blockers.join(" | ")}`)
  if (!bits.length) return ""
  const system = "Jesteś asystentem dev. Podsumuj krótko stan sesji (1-2 zdania po polsku)."
  const prompt = `Stan sesji:\n${bits.join("\n")}\n\nPodsumuj krótko co zrobiono i na czym stiano:`
  const out = await aiComplete({ system, prompt, label: "summarize session" })
  return out ?? ""
}

// --- AI-enhanced: ekstrakcja konwencji/ryzyka z README/CLAUDE.md ---------------
// Zwraca listę faktów "ludzkich" wykrytych z dokumentacji. Fallback: puste (auto-fakty deterministyczne wystarczą).
// Zmiana: wywołanie tylko gdy pliki źródłowe uległy zmianie (hash w cache). Bez throttlingu czasowego.

function aiFactsHashPath(): string {
  return join(memoryDir, "cache", "ai-facts-hash.json")
}

function computeDocsHash(root: string): string {
  const candidates = ["README.md", "CLAUDE.md", "AGENTS.md", "CONTRIBUTING.md"]
  const h = createHash("sha256")
  for (const f of candidates) {
    const p = join(root, f)
    if (existsSync(p)) h.update(readText(p))
  }
  return h.digest("hex")
}

function aiHumanFactsChanged(root: string): boolean {
  const hash = computeDocsHash(root)
  const data = readJson<{ hash?: string } | null>(aiFactsHashPath())
  if (data?.hash === hash) return false
  try { writeJson(aiFactsHashPath(), { hash }) } catch { /* swallow */ }
  return true
}

async function aiExtractHumanFacts(root: string): Promise<string> {
  if (!aiHumanFactsChanged(root)) return ""
  const candidates = ["README.md", "CLAUDE.md", "AGENTS.md", "CONTRIBUTING.md"]
  const docs: string[] = []
  for (const f of candidates) {
    const p = join(root, f)
    if (existsSync(p)) {
      const raw = readText(p)
      if (raw) docs.push(`## ${f}\n${truncateLines(raw, 200)}`)
    }
  }
  if (!docs.length) return ""
  const system = "Ekstrahuj konwencje, ryzyka i decyzje architektoniczne z dokumentacji projektu. Odpowiadaj po polsku w formacie Markdown z sekcjami ## Konwencje, ## Ryzyka. Brak = puste sekcje."
  const prompt = `Dokumentacja projektu:\n\n${docs.join("\n\n")}\n\nWymień konwencje kodowania, ryzyka i decyzje architektoniczne (max 10 punktów łącznie):`
  const out = await aiComplete({ system, prompt, label: "extract human facts" })
  return out ?? ""
}

// ---------------------------------------------------------------------------
// Memory layout
// ---------------------------------------------------------------------------

// Opencode bez repo git przekazuje worktree="/" (Windows: path.join rozwija
// do bieżącego dysku, np. D:\.opencode\memory — poza projektem). Wykrywaj
// bezużyteczny worktree i fallbackuj na directory/process.cwd().
function resolveWorktree(worktree: string | undefined, directory?: string): string {
  const isBogus = (p: string | undefined): boolean => {
    if (!p) return true
    const norm = p.replace(/\\/g, "/")
    if (norm === "/" || norm === "" ) return true
    // root dysku Windows: "D:","D:/","D:\\"
    if (/^[A-Za-z]:\/?$/ .test(norm)) return true
    return false
  }
  if (!isBogus(worktree)) return worktree as string
  if (!isBogus(directory)) return directory as string
  return process.cwd()
}

function initMemoryLayout(worktree: string) {
  worktreePath = worktree
  projectRoot = worktree
  memoryDir = join(worktree, ".opencode", "memory")
  ensureDir(memoryDir)
  ensureDir(join(memoryDir, "session-history"))
  ensureDir(join(memoryDir, "artifacts"))
  ensureDir(join(memoryDir, "cache"))
  ensureDir(join(memoryDir, "index"))
  // Additions: load persistent dedup cache, test history, session trace
  loadDedupCache()
  loadTestHistory()
  loadSessionTrace()
  // AI: load persistent max observed prompt duration (for dynamic timeout)
  aiStatus.maxObservedMs = aiLoadMaxObserved()
}

function factsPath(): string {
  return join(memoryDir, "project-facts.md")
}

// --- Auto-extracted facts -----------------------------------------------------
// Deterministyczne ekstraktory czytają repozytorium i budują project-facts.auto.md.
// Plik .auto.md jest regenerowany; project-facts.md pozostaje dla faktów ręcznych.

function factsAutoPath(): string {
  return join(memoryDir, "project-facts.auto.md")
}

const IGNORED_DIRS = new Set([
  "node_modules", ".git", ".ijfw", ".opencode", "dist", "build", "out",
  ".next", ".nuxt", ".cache", ".turbo", "target", "bin", "obj",
  "__pycache__", ".venv", "venv", "vendor", ".idea", ".vscode",
])

function listTopDirs(root: string, depth: number): string[] {
  const out: string[] = []
  const walk = (dir: string, d: number) => {
    if (d > depth) return
    let entries: string[] = []
    try { entries = readdirSync(dir) } catch { return }
    for (const e of entries) {
      const full = join(dir, e)
      let st
      try { st = statSync(full) } catch { continue }
      if (!st.isDirectory()) continue
      if (IGNORED_DIRS.has(e)) continue
      const rel = relative(root, full).replace(/\\/g, "/")
      out.push(rel)
      walk(full, d + 1)
    }
  }
  walk(root, 1)
  return out.sort()
}

function readJsonManifest(root: string, file: string): any | null {
  const p = join(root, file)
  if (!existsSync(p)) return null
  try { return JSON.parse(readFileSync(p, "utf8")) } catch { return null }
}

function extractBuildAndTestCommands(root: string): { build: string[]; test: string[]; format: string[]; lint: string[] } {
  const build: string[] = []
  const test: string[] = []
  const format: string[] = []
  const lint: string[] = []
  // package.json (npm/bun/yarn/pnpm)
  const pkg = readJsonManifest(root, "package.json")
  if (pkg && pkg.scripts) {
    const s = pkg.scripts as Record<string, string>
    const push = (arr: string[], k: string, label: string) => {
      if (s[k]) arr.push(`npm run ${k}  (package.json: ${s[k]})`)
    }
    for (const k of ["build", "build:debug", "compile", "tsc"]) push(build, k, "build")
    for (const k of ["test", "test:unit", "test:ci", "vitest", "jest"]) push(test, k, "test")
    for (const k of ["format", "prettier", "lint:fix"]) push(format, k, "format")
    for (const k of ["lint", "eslint", "biome", "tsc --noEmit"]) push(lint, k, "lint")
    // packageManager hint
    if (pkg.packageManager) build.push(`# packageManager: ${pkg.packageManager}`)
  }
  // pyproject.toml / setup.py
  const pyproject = join(root, "pyproject.toml")
  if (existsSync(pyproject)) {
    const raw = readText(pyproject)
    if (/\[tool\.pytest\]/.test(raw) || /pytest/.test(raw)) test.push("pytest  (pyproject.toml)")
    if (/\[tool\.black\]/.test(raw) || /\[tool\.ruff\]/.test(raw)) {
      if (/\[tool\.ruff\]/.test(raw)) { format.push("ruff format  (pyproject.toml)"); lint.push("ruff check  (pyproject.toml)") }
      if (/\[tool\.black\]/.test(raw)) format.push("black  (pyproject.toml)")
    }
    if (/\[tool\.mypy\]/.test(raw)) lint.push("mypy  (pyproject.toml)")
    if (/\[project\.scripts\]/.test(raw) || /\[tool\.poetry\]/.test(raw)) {
      const m = raw.match(/build-system[\s\S]*?requires\s*=\s*\[([^\]]+)\]/)
      if (m) build.push(`# build-backend: ${m[1].replace(/[\n"']/g, " ").trim()}`)
    }
  }
  // Makefile
  const makefile = join(root, "Makefile")
  if (existsSync(makefile)) {
    const raw = readText(makefile)
    const targets = new Set<string>()
    for (const line of raw.split("\n")) {
      const m = line.match(/^([a-zA-Z0-9_\-]+):\s*/)
      if (m) targets.add(m[1])
    }
    for (const t of ["build", "all", "compile", "debug"]) if (targets.has(t)) build.push(`make ${t}  (Makefile)`)
    for (const t of ["test", "check", "test-unit"]) if (targets.has(t)) test.push(`make ${t}  (Makefile)`)
    for (const t of ["lint", "format", "fmt"]) if (targets.has(t)) (t === "lint" ? lint : format).push(`make ${t}  (Makefile)`)
  }
  // CMake
  const cmake = join(root, "CMakeLists.txt")
  if (existsSync(cmake)) {
    build.push("cmake --build build  (CMakeLists.txt)")
    const raw = readText(cmake)
    if (/enable_testing|add_test|gtest|Catch2|catch2/i.test(raw)) test.push("ctest --test-dir build  (CMakeLists.txt)")
  }
  // Cargo
  const cargo = join(root, "Cargo.toml")
  if (existsSync(cargo)) {
    build.push("cargo build  (Cargo.toml)")
    test.push("cargo test  (Cargo.toml)")
    format.push("cargo fmt  (Cargo.toml)")
    lint.push("cargo clippy  (Cargo.toml)")
  }
  // Go
  if (existsSync(join(root, "go.mod"))) {
    build.push("go build ./...  (go.mod)")
    test.push("go test ./...  (go.mod)")
    format.push("gofmt -w .  (go.mod)")
  }
  // dotnet: *.csproj/*.sln
  try {
    const hasCs = readdirSync(root).some((f) => /\.(csproj|sln|fsproj|vbproj)$/i.test(f))
    if (hasCs) {
      build.push("dotnet build  (*.csproj)")
      test.push("dotnet test  (*.csproj)")
      format.push("dotnet format  (*.csproj)")
    }
  } catch { /* ignore */ }
  // Dedup preserving order
  const uniq = (a: string[]) => Array.from(new Set(a))
  return { build: uniq(build), test: uniq(test), format: uniq(format), lint: uniq(lint) }
}

function extractEnvironment(root: string): string[] {
  const out: string[] = []
  const readLine = (file: string): string | null => {
    const p = join(root, file)
    if (!existsSync(p)) return null
    const raw = readText(p).split("\n")[0]?.trim()
    return raw || null
  }
  const node = readLine(".nvmrc") ?? readLine(".node-version")
  if (node) out.push(`Node: ${node}  (.nvmrc)`)
  const py = readLine(".python-version")
  if (py) out.push(`Python: ${py}  (.python-version)`)
  const ruby = readLine(".ruby-version")
  if (ruby) out.push(`Ruby: ${ruby}  (.ruby-version)`)
  // mise / asdf / tool-versions
  const tv = join(root, ".tool-versions")
  if (existsSync(tv)) {
    for (const l of readText(tv).split("\n")) {
      const m = l.match(/^(\w+)\s+(\S+)/)
      if (m) out.push(`${m[1]}: ${m[2]}  (.tool-versions)`)
    }
  }
  const mise = join(root, "mise.toml")
  if (existsSync(mise)) {
    for (const l of readText(mise).split("\n")) {
      const m = l.match(/^\s*(\w+)\s*=\s*["']?([^"'\s]+)["']?/)
      if (m && !["env", "tasks"].includes(m[1])) out.push(`${m[1]}: ${m[2]}  (mise.toml)`)
    }
  }
  // Dockerfile
  if (existsSync(join(root, "Dockerfile"))) {
    const raw = readText(join(root, "Dockerfile"))
    const fm = raw.match(/FROM\s+([^\s]+)/i)
    if (fm) out.push(`Container base: ${fm[1]}  (Dockerfile)`)
  }
  // Host OS + WSL hint (deterministic, no shell call)
  const hostOs = process.platform
  const hostOsLabel = hostOs === "win32" ? "Windows" : hostOs === "linux" ? "Linux" : hostOs === "darwin" ? "macOS" : hostOs
  out.push(`Host OS: ${hostOsLabel}  (process.platform=${hostOs})`)
  if (hostOs === "linux" && existsSync("/proc/sys/fs/binfmt_misc/WSLInterop")) {
    out.push("Runtime: WSL (binfmt WSLInterop wykryty)")
  }
  // WSL detection on Windows: sprawdz presence wsl.exe w PATH + skrypty *.sh obok *.ps1
  if (hostOs === "win32") {
    const hasSh = existsSync(join(root, "install.sh")) || existsSync(join(root, "scripts", "install.sh"))
    const hasPs1 = existsSync(join(root, "install.ps1")) || existsSync(join(root, "scripts", "install.ps1"))
    if (hasSh && hasPs1) out.push("Build env: cross-platform shell (WSL + PowerShell)  (*.sh + *.ps1)")
    else if (hasSh) out.push("Build env: WSL/bash wymagany  (*.sh bez *.ps1)")
  }
  return out
}

// --- Target platform (CI matrix, desktop/mobile wrappers, tsconfig lib) --------
function extractTargetPlatform(root: string): string[] {
  const out: string[] = []
  // GitHub Actions matrix.os / runs-on
  const ghDir = join(root, ".github", "workflows")
  if (existsSync(ghDir)) {
    try {
      const osSet = new Set<string>()
      for (const f of readdirSync(ghDir)) {
        if (!f.endsWith(".yml") && !f.endsWith(".yaml")) continue
        const raw = readText(join(ghDir, f))
        // runs-on: ubuntu-latest / matrix.os: [ubuntu-latest, windows-latest, macos-latest]
        for (const m of raw.matchAll(/(?:runs-on|os):\s*\[?["'`]?([a-z]+-latest)["'`\]?,]?/gi)) {
          if (m[1]) osSet.add(m[1])
        }
      }
      if (osSet.size) out.push(`CI matrix: ${Array.from(osSet).join(", ")}  (.github/workflows)`)
    } catch { /* ignore */ }
  }
  // Desktop/mobile wrappers
  if (existsSync(join(root, "electron-builder.yml")) || existsSync(join(root, "electron-builder.json")) || existsSync(join(root, "electron-builder.config.js"))) {
    out.push("Target: Electron desktop  (electron-builder)")
  }
  if (existsSync(join(root, "tauri.conf.json")) || existsSync(join(root, "src-tauri", "tauri.conf.json"))) {
    out.push("Target: Tauri desktop  (tauri.conf.json)")
  }
  if (existsSync(join(root, "capacitor.config.ts")) || existsSync(join(root, "capacitor.config.json"))) {
    out.push("Target: Capacitor mobile  (capacitor.config)")
  }
  if (existsSync(join(root, "expo.json")) || existsSync(join(root, "app.json"))) {
    const aj = readJsonManifest(root, "app.json")
    if (aj?.expo) out.push("Target: Expo/React Native mobile  (app.json expo)")
  }
  // package.json os/cpu constraints + bin hints
  const pkg = readJsonManifest(root, "package.json")
  if (pkg) {
    if (Array.isArray(pkg.os) && pkg.os.length) out.push(`Target os: ${pkg.os.join(", ")}  (package.json os)`)
    if (Array.isArray(pkg.cpu) && pkg.cpu.length) out.push(`Target cpu: ${pkg.cpu.join(", ")}  (package.json cpu)`)
    if (pkg.main && /^dist\/(electron|tauri|desktop)/.test(pkg.main)) out.push(`Target: desktop  (package.json main=${pkg.main})`)
  }
  // tsconfig.json lib (browser vs node target)
  const tscfg = join(root, "tsconfig.json")
  if (existsSync(tscfg)) {
    try {
      const raw = readText(tscfg)
      const libMatch = raw.match(/"lib"\s*:\s*\[([^\]]+)\]/)
      if (libMatch) {
        const libs = libMatch[1].replace(/["' ]/g, "").split(",").filter(Boolean)
        const hasDom = libs.some((l) => l.toLowerCase().startsWith("dom"))
        const hasNode = libs.some((l) => l.toLowerCase().includes("node"))
        if (hasDom && hasNode) out.push("tsconfig lib: DOM+Node  (hybrid/browser+node)")
        else if (hasDom) out.push("tsconfig lib: DOM  (browser target)")
        else if (hasNode) out.push("tsconfig lib: Node  (node target)")
      }
    } catch { /* ignore */ }
  }
  return out
}

function extractArchitecture(root: string): { stack: string[]; dirs: string[] } {
  const stack: string[] = []
  const dirs = listTopDirs(root, cfg.factsAutoGlobDepth)
  // wykrywanie stacku po plikach manifestu i dominujących rozszerzeniach
  const extCounts: Record<string, number> = {}
  const walk = (dir: string, d: number) => {
    if (d > cfg.factsAutoGlobDepth) return
    let entries: string[] = []
    try { entries = readdirSync(dir) } catch { return }
    for (const e of entries) {
      const full = join(dir, e)
      let st
      try { st = statSync(full) } catch { continue }
      if (st.isDirectory()) {
        if (IGNORED_DIRS.has(e)) continue
        walk(full, d + 1)
      } else {
        const ext = e.includes(".") ? e.slice(e.lastIndexOf(".")) : ""
        if (ext) extCounts[ext] = (extCounts[ext] ?? 0) + 1
      }
    }
  }
  walk(root, 1)
  if (existsSync(join(root, "package.json"))) stack.push("TypeScript/JavaScript (Node)")
  if (existsSync(join(root, "tsconfig.json"))) stack.push("TypeScript (tsc)")
  if (existsSync(join(root, "Cargo.toml"))) stack.push("Rust (Cargo)")
  if (existsSync(join(root, "go.mod"))) stack.push("Go")
  if (existsSync(join(root, "pom.xml")) || existsSync(join(root, "build.gradle")) || existsSync(join(root, "build.gradle.kts"))) stack.push("Java/Kotlin (JVM)")
  if (existsSync(join(root, "pyproject.toml")) || existsSync(join(root, "setup.py")) || existsSync(join(root, "requirements.txt"))) stack.push("Python")
  if (existsSync(join(root, "CMakeLists.txt")) || existsSync(join(root, "Makefile"))) stack.push("C/C++ (native)")
  // dominujące rozszerzenia jako hint
  const top = Object.entries(extCounts).sort((a, b) => b[1] - a[1]).slice(0, 5)
  for (const [ext, n] of top) {
    if (n < 3) continue
    if (ext === ".ts" || ext === ".tsx" || ext === ".js" || ext === ".jsx") continue
    stack.push(`${ext} (${n} plików)`)
  }
  return { stack: Array.from(new Set(stack)), dirs }
}

function buildAutoFacts(): string {
  const root = worktreePath || projectRoot || process.cwd()
  const cmds = extractBuildAndTestCommands(root)
  const env = extractEnvironment(root)
  const platform = extractTargetPlatform(root)
  const arch = extractArchitecture(root)
  const out: string[] = []
  out.push("# project-facts.auto.md — generowane automatycznie przez plugin")
  out.push("# Nie edytuj ręcznie; plik jest regenerowany na session.idle/compacted.")
  out.push(`# Ostatnia aktualizacja: ${new Date().toISOString()}`)
  out.push("")
  if (arch.stack.length) {
    out.push("## Architektura")
    for (const s of arch.stack) out.push(`- ${s}`)
    if (arch.dirs.length) out.push(`- Główne katalogi: ${arch.dirs.slice(0, 15).join(", ")}`)
    out.push("")
  }
  if (cmds.build.length || cmds.test.length || cmds.format.length || cmds.lint.length) {
    out.push("## Komendy")
    for (const c of cmds.build) out.push(`- Build: ${c}`)
    for (const c of cmds.test) out.push(`- Testy: ${c}`)
    for (const c of cmds.format) out.push(`- Formatowanie: ${c}`)
    for (const c of cmds.lint) out.push(`- Lint: ${c}`)
    out.push("")
  }
  if (env.length) {
    out.push("## Środowisko")
    for (const e of env) out.push(`- ${e}`)
    out.push("")
  }
  if (platform.length) {
    out.push("## Platforma docelowa")
    for (const p of platform) out.push(`- ${p}`)
    out.push("")
  }
  return out.join("\n")
}

function refreshAutoFacts(): void {
  const body = buildAutoFacts()
  writeFileSync(factsAutoPath(), body, "utf8")
}

function readAutoFacts(): string {
  const raw = readText(factsAutoPath())
  if (!raw) return ""
  const tokens = estimateTokens(raw)
  if (tokens > cfg.maxProjectMemoryTokens) {
    return truncateByBytes(raw, cfg.maxProjectMemoryTokens * 4) + "\n\n[WARN: project-facts.auto.md exceeds memory budget; truncated]"
  }
  return raw
}

function activeSessionPath(): string {
  return join(memoryDir, "active-session.json")
}

function artifactsDir(): string {
  return join(memoryDir, "artifacts")
}

function cachePath(): string {
  return join(memoryDir, "cache", "tool-results.json")
}

function indexFilesPath(): string {
  return join(memoryDir, "index", "files.json")
}

function metricsPath(): string {
  return join(memoryDir, "cache", "metrics.json")
}

// --- Additions: paths for new persistent data ---------------------------------
function dedupCachePath(): string {
  return join(memoryDir, "cache", "dedup-seen.json")
}

function testHistoryPath(): string {
  return join(memoryDir, "cache", "test-history.json")
}

function sessionTracePath(): string {
  return join(memoryDir, "cache", "session-trace.json")
}

function proposedFactsPath(): string {
  return join(memoryDir, "cache", "proposed-facts.md")
}

// --- Additions: load / save persistent dedup cache ---------------------------
function loadDedupCache() {
  if (!cfg.persistentDedupCache) return
  const data = readJson<SeenContext[]>(dedupCachePath())
  if (Array.isArray(data)) {
    seen = new Map(data.map((s) => [dedupKey(s), s]))
  }
}

function saveDedupCache() {
  if (!cfg.persistentDedupCache) return
  // LRU eviction by deliveredAt (oldest first) when over capacity
  if (seen.size > cfg.maxDedupCacheEntries) {
    const entries = Array.from(seen.values()).sort((a, b) => (a.deliveredAt < b.deliveredAt ? -1 : 1))
    seen = new Map(entries.slice(entries.length - cfg.maxDedupCacheEntries).map((s) => [dedupKey(s), s]))
  }
  writeJson(dedupCachePath(), Array.from(seen.values()))
}

// --- Additions: load / save test history --------------------------------------
function loadTestHistory() {
  const data = readJson<TestRun[]>(testHistoryPath())
  if (Array.isArray(data)) testHistory = data
}

function recordTestRun(run: TestRun) {
  testHistory.push(run)
  // keep most recent N
  if (testHistory.length > cfg.maxTestHistoryEntries) {
    testHistory = testHistory.slice(testHistory.length - cfg.maxTestHistoryEntries)
  }
  writeJson(testHistoryPath(), testHistory)
}

// --- Additions: load / save session trace (aggregated across sessions) --------
function loadSessionTrace() {
  const data = readJson<SessionTrace>(sessionTracePath())
  if (data && typeof data === "object") {
    sessionTrace = data
  }
}

function saveSessionTrace() {
  writeJson(sessionTracePath(), sessionTrace)
}

function mergeTraceIntoGlobal() {
  // On session.idle, fold the per-session trace into the persistent trace,
  // then persist. buildTestCommands & editedFiles accumulate counts across sessions.
  const globalRaw = readJson<SessionTrace>(sessionTracePath())
  const g: SessionTrace = globalRaw && typeof globalRaw === "object" ? globalRaw : {
    sessionId: "", buildTestCommands: {}, editedFiles: {}, blockers: [], startedAt: "",
  }
  for (const [cmd, n] of Object.entries(sessionTrace.buildTestCommands)) {
    g.buildTestCommands[cmd] = (g.buildTestCommands[cmd] ?? 0) + n
  }
  for (const [f, n] of Object.entries(sessionTrace.editedFiles)) {
    g.editedFiles[f] = (g.editedFiles[f] ?? 0) + n
  }
  for (const b of sessionTrace.blockers) {
    if (!g.blockers.includes(b)) g.blockers.push(b)
  }
  g.buildTestCommands = trimObject(g.buildTestCommands, 20)
  g.editedFiles = trimObject(g.editedFiles, 40)
  g.blockers = g.blockers.slice(-20)
  writeJson(sessionTracePath(), g)
}

function trimObject(obj: Record<string, number>, max: number): Record<string, number> {
  const entries = Object.entries(obj)
  if (entries.length <= max) return obj
  entries.sort((a, b) => b[1] - a[1])
  return Object.fromEntries(entries.slice(0, max))
}

// --- Additions: detect build/test commands from a bash command string ---------
const BUILD_TEST_RE = /\b(pytest|npm test|idf\.py build|idf\.py test|cmake --build|make|cargo test|jest|vitest|go test|mvn test|gradle test)\b/

function isBuildTestCommand(command: string): boolean {
  return BUILD_TEST_RE.test(command)
}

function normalizeBuildTestCommand(command: string): string {
  // strip arguments after the tool name for stable aggregation
  const m = command.match(/\b(pytest|npm test|idf\.py build|idf\.py test|cmake --build|make|cargo test|jest|vitest|go test|mvn test|gradle test)\b/)
  return m ? m[1] : command.split(/\s+/).slice(0, 2).join(" ")
}

// --- Additions: parse failed test names from build/test output ----------------
function parseFailedTests(result: string, command: string): string[] {
  const failed: string[] = []
  // pytest: "FAILED tests/test_retry.py::test_name"
  if (/pytest/.test(command)) {
    const re = /FAILED\s+(\S+)/g
    let m: RegExpExecArray | null
    while ((m = re.exec(result)) && failed.length < 30) failed.push(m[1])
  }
  // jest/vitest: "✕ test name (X ms)" or "FAIL  path/test.js"
  if (/jest|vitest/.test(command)) {
    const re = /FAIL\s+(\S+)/g
    let m: RegExpExecArray | null
    while ((m = re.exec(result)) && failed.length < 30) failed.push(m[1])
  }
  // cargo: "test result: FAILED. ...", or "failures: name"
  if (/cargo test/.test(command)) {
    const re = /failures:\n([\s\S]*?)\n\n/
    const block = re.exec(result)?.[1] ?? ""
    for (const l of block.split("\n").filter(Boolean)) failed.push(l.trim())
  }
  // generic fallback: lines with FAIL/error
  if (failed.length === 0) {
    for (const l of result.split("\n")) {
      if (/^FAIL\b|^FAILED\b|: error:/i.test(l) && failed.length < 20) failed.push(l.trim().slice(0, 200))
    }
  }
  return failed
}

function parseTestSummary(result: string): string {
  for (const l of result.split("\n")) {
    if (/passed|failed|PASS|tests?\s+\d/i.test(l)) return l.trim()
  }
  return ""
}

// --- Additions: propose facts from session trace -----------------------------
function buildProposedFacts(): string {
  const gRaw = readJson<SessionTrace>(sessionTracePath())
  const g: SessionTrace | null = gRaw && typeof gRaw === "object" ? gRaw : null
  const out: string[] = ["# Proponowane fakty projektu (wygenerowane przez plugin)", ""]
  let added = false

  if (g && Object.keys(g.buildTestCommands).length) {
    added = true
    out.push("## Komendy")
    const cmds = Object.entries(g.buildTestCommands).sort((a, b) => b[1] - a[1])
    for (const [cmd, n] of cmds) {
      out.push(`- ${cmd}  (uruchamiano ${n}× w sesjach)`)
    }
    out.push("")
  }

  if (g && Object.keys(g.editedFiles).length) {
    added = true
    out.push("## Hotspoty (często edytowane pliki)")
    const files = Object.entries(g.editedFiles).sort((a, b) => b[1] - a[1]).slice(0, 10)
    for (const [f, n] of files) {
      out.push(`- ${f}  (${n}× edytowany)`)
    }
    out.push("")
  }

  // recent test failures from test history
  const recentFails = testHistory
    .filter((t) => t.exitCode !== 0)
    .slice(-5)
  if (recentFails.length) {
    added = true
    out.push("## Ostatnie nieudane testy")
    for (const t of recentFails) {
      out.push(`- [${t.timestamp}] ${t.command} (exit ${t.exitCode}): ${t.failed.slice(0, 3).join(", ") || t.summary}`)
    }
    out.push("")
  }

  if (g && g.blockers.length) {
    added = true
    out.push("## Ryzyka / powtarzające się blokery")
    for (const b of g.blockers.slice(-10)) out.push(`- ${b}`)
    out.push("")
  }

  // Heurystyka trudnych problemów: ≥3 uruchomienia tego samego command, z czego
  // przynajmniej jedno nieudane (exitCode!=0) i ostatnie udane (exitCode==0).
  // Sugeruje, że problem rozwiązano po iteracjach — warte zapisania lekcji.
  const byCmd: Record<string, TestRun[]> = {}
  for (const t of testHistory) {
    const key = t.command
    if (!byCmd[key]) byCmd[key] = []
    byCmd[key].push(t)
  }
  const hardProblems: string[] = []
  for (const [cmd, runs] of Object.entries(byCmd)) {
    if (runs.length < 3) continue
    const hasFail = runs.some((r) => r.exitCode !== 0)
    const lastOk = runs[runs.length - 1].exitCode === 0
    if (!hasFail || !lastOk) continue
    // zbierz unikalne failed test names z nieudanych runów
    const failedNames = new Set<string>()
    for (const r of runs) {
      if (r.exitCode !== 0) for (const f of r.failed) failedNames.add(f)
    }
    const firstTs = runs[0].timestamp.slice(0, 10)
    const lastTs = runs[runs.length - 1].timestamp.slice(0, 10)
    const span = firstTs === lastTs ? firstTs : `${firstTs}..${lastTs}`
    const failedBit = failedNames.size ? ` (failed: ${Array.from(failedNames).slice(0, 3).join(", ")})` : ""
    hardProblems.push(`- ${cmd}: rozwiązano po ${runs.length} iteracjach [${span}]${failedBit}`)
  }
  if (hardProblems.length) {
    added = true
    out.push("## Trudne problemy (heurystyka: ≥3 iteracje build/test → sukces)")
    out.push(...hardProblems)
    out.push("")
    out.push("> Jeśli któryś z tych problemów był nieoczywisty, rozważ: /codemem lesson <krótki opis problem+rozwiązanie+why>")
    out.push("")
  }

  if (!added) {
    out.push("(brak danych — uruchom kilka sesji z buildem/testami i edycją plików, aby plugin zebrał statystyki)")
  }
  return out.join("\n")
}

function commitProposedFacts(): string {
  const proposed = buildProposedFacts()
  const existing = readText(factsPath())
  const sep = existing.endsWith("\n") ? "\n" : existing ? "\n\n" : ""
  const merged = existing + sep + "\n<!-- === propozycje pluginu (dodane /codemem commit) === -->\n" + proposed
  writeFileSync(factsPath(), merged, "utf8")
  // clear the proposed buffer by resetting trace (keep history though)
  rmSync(proposedFactsPath(), { force: true })
  return "Propozycje dopisane do project-facts.md. Przejrzyj i edytuj ręcznie, aby zachować zwięzłość."
}

// --- /codemem lesson: ręczne dopisywanie lekcji (trudne problemy, decisions) ---
// Lekcje lądują w wersjonowanym project-facts.md w sekcji "## Lekcje".
// Krótkie, z datą — dla przyszłych sesji, które natrafiają na podobny problem.
function memoryLesson(text: string): string {
  const t = (text || "").trim()
  if (!t) return "Użycie: /codemem lesson <opis lekcji>. Krótko: problem + rozwiązanie + dlaczego."
  if (t.length > 600) return "Lekcja za długa (>600 znaków). Skróć do istotnego problem+rozwiązanie+why."
  const existing = readText(factsPath())
  const ts = new Date().toISOString().slice(0, 10)
  const line = `- [${ts}] ${t}`
  if (existing.includes("## Lekcje")) {
    // dopisz pod istniejącą sekcją
    const idx = existing.indexOf("## Lekcje")
    const nextHeader = existing.indexOf("\n## ", idx + 1)
    const insertAt = nextHeader === -1 ? existing.length : nextHeader
    const before = existing.slice(0, insertAt).replace(/\n*$/, "\n")
    const after = existing.slice(insertAt)
    const merged = before + line + "\n" + (after.startsWith("\n") ? after : "\n" + after)
    writeFileSync(factsPath(), merged, "utf8")
  } else {
    const sep = existing.endsWith("\n") ? "\n" : existing ? "\n\n" : ""
    const header = existing ? "## Lekcje\n" : "# project-facts.md\n\n## Lekcje\n"
    writeFileSync(factsPath(), existing + sep + header + line + "\n", "utf8")
  }
  return `Lekcja dopisana do project-facts.md (## Lekcje):\n${line}`
}

// ---------------------------------------------------------------------------
// project-facts.md
// ---------------------------------------------------------------------------

function factsAiPath(): string {
  return join(memoryDir, "project-facts.ai.md")
}

function readProjectFacts(): string {
  const raw = readText(factsPath())
  const auto = cfg.autoExtractFacts ? readAutoFacts() : ""
  const ai = cfg.ai?.enabled ? readText(factsAiPath()) : ""
  const merged = [auto, ai, raw].filter(Boolean).join("\n\n---\n\n")
  if (!merged) return ""
  const tokens = estimateTokens(merged)
  if (tokens > cfg.maxProjectMemoryTokens) {
    const maxBytes = cfg.maxProjectMemoryTokens * 4
    const truncated = truncateByBytes(merged, maxBytes)
    return truncated + "\n\n[WARN: project-facts exceeds memory budget; truncated]"
  }
  return merged
}

// ---------------------------------------------------------------------------
// active-session.json
// ---------------------------------------------------------------------------

function readActiveSession(): ActiveSession | null {
  return readJson<ActiveSession>(activeSessionPath())
}

function writeActiveSession(s: ActiveSession) {
  s.updatedAt = new Date().toISOString()
  // token-budget guard: if serialized too large, trim fields
  let serialized = JSON.stringify(s)
  if (estimateTokens(serialized) > cfg.maxSessionHandoffTokens) {
    s.decisions = s.decisions.slice(0, 3)
    s.modifiedFiles = s.modifiedFiles.slice(0, 10)
    s.blockers = s.blockers.slice(0, 3)
    s.lspErrors = s.lspErrors?.slice(0, 5)
    serialized = JSON.stringify(s)
  }
  writeJson(activeSessionPath(), s)
  // also persist per-session history
  if (s.sessionId) {
    writeJson(join(memoryDir, "session-history", `${s.sessionId}.json`), s)
  }
}

// ---------------------------------------------------------------------------
// Git helpers (via Bun shell $)
// ---------------------------------------------------------------------------

async function gitInfo($: any): Promise<{ branch: string; head: string; modified: string[]; recentDiff: string[] }> {
  const empty = { branch: "", head: "", modified: [], recentDiff: [] }
  // `$` (Bun shell) may be unavailable in some plugin runtimes — fall back to execSync
  if (typeof $ !== "function") {
    try {
      const branch = execSync(`git -C ${JSON.stringify(worktreePath)} rev-parse --abbrev-ref HEAD`, { encoding: "utf8" }).trim() || "(detached)"
      const head = execSync(`git -C ${JSON.stringify(worktreePath)} rev-parse --short HEAD`, { encoding: "utf8" }).trim()
      const recentDiff = (gitFilesChangedBetween("HEAD~20", "HEAD").length
        ? gitFilesChangedBetween("HEAD~20", "HEAD")
        : gitFilesChangedBetween("", "HEAD")).slice(0, 20)
      return { branch, head, modified: gitStatusPorcelain(), recentDiff }
    } catch {
      return empty
    }
  }
  try {
    const branch = (await $`git -C ${worktreePath} rev-parse --abbrev-ref HEAD`.text()).trim() || "(detached)"
    const head = (await $`git -C ${worktreePath} rev-parse --short HEAD`.text()).trim()
    const modifiedRaw = (await $`git -C ${worktreePath} status --porcelain`.text()).trim()
    const modified = modifiedRaw
      .split("\n")
      .filter(Boolean)
      .map((l: string) => l.slice(3).trim())
    const diffRaw = (await $`git -C ${worktreePath} diff --name-only HEAD~20 2>/dev/null || git -C ${worktreePath} diff --name-only HEAD 2>/dev/null || true`.text()).trim()
    const recentDiff = diffRaw.split("\n").filter(Boolean).slice(0, 20)
    return { branch, head, modified, recentDiff }
  } catch {
    return empty
  }
}

// ---------------------------------------------------------------------------
// Regression detection helpers (sync, via execSync)
// ---------------------------------------------------------------------------

function getHeadSha(): string {
  try {
    return execSync(`git -C ${JSON.stringify(worktreePath)} rev-parse HEAD`, { encoding: "utf8" }).trim()
  } catch {
    return ""
  }
}

function gitFilesChangedBetween(fromSha: string, toSha: string): string[] {
  try {
    const range = fromSha && toSha ? `${fromSha}..${toSha}` : toSha || "HEAD"
    const out = execSync(`git -C ${JSON.stringify(worktreePath)} diff --name-only ${range}`, { encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] })
    return out.split("\n").filter(Boolean)
  } catch {
    return []
  }
}

function gitCommitsBetween(fromSha: string, toSha: string): string[] {
  try {
    const range = fromSha && toSha ? `${fromSha}..${toSha}` : toSha || "HEAD"
    const out = execSync(`git -C ${JSON.stringify(worktreePath)} log --oneline ${range}`, { encoding: "utf8" })
    return out.split("\n").filter(Boolean)
  } catch {
    return []
  }
}

function gitStatusPorcelain(): string[] {
  try {
    const out = execSync(`git -C ${JSON.stringify(worktreePath)} status --porcelain`, { encoding: "utf8" })
    return out.split("\n").filter(Boolean).map((l) => l.slice(3).trim())
  } catch {
    return []
  }
}

function gitStash(args: string): string {
  try {
    return execSync(`git -C ${JSON.stringify(worktreePath)} stash ${args}`, { encoding: "utf8" }).trim()
  } catch (e: any) {
    return `git stash ${args} failed: ${e?.message ?? e}`
  }
}

function gitCheckoutFileFromSha(sha: string, file: string): string {
  try {
    execSync(`git -C ${JSON.stringify(worktreePath)} checkout ${sha} -- ${JSON.stringify(file)}`, { encoding: "utf8" })
    return `Przywrócono ${file} do wersji ${sha.slice(0, 7)}.`
  } catch (e: any) {
    return `Nie udało się przywrócić ${file}: ${e?.message ?? e}`
  }
}

// --- Feature marks via git notes ----------------------------------------------
// Feature = zdefiniowana nazwa opcji/mechanizmu. Oznaczenie „działa poprawnie"
// zapisujemy jako notę (git notes) na konkretnym commicie, więc znika potrzeba
// przepisywania historii — punkt odniesienia = commit, na którym feature został
// oznaczony. Notesy kumulujemy przez `git notes append` (jedna nota na commit).

function gitNotesAppend(sha: string, message: string): string {
  // Uwaga (Windows): przekazywanie nowych linii przez -m nie jest pewne (shell
  // zamienia je na literalne \n). Dlatego nota idzie przez plik tymczasowy (-F).
  const tmpFile = join(tmpdir(), `opencode-note-${createHash("sha1").update(sha + Date.now()).digest("hex").slice(0, 16)}.txt`)
  try {
    writeFileSync(tmpFile, message, "utf8")
    execSync(
      `git -C ${JSON.stringify(worktreePath)} notes append -F ${JSON.stringify(tmpFile)} ${sha}`,
      { encoding: "utf8", stdio: "pipe" },
    )
    return `Oznaczono ${sha.slice(0, 7)}.`
  } catch (e: any) {
    return `git notes append failed: ${e?.message ?? e}`
  } finally {
    try { unlinkSync(tmpFile) } catch { /* ignore */ }
  }
}

function gitNotesShow(sha: string): string {
  try {
    return execSync(
      `git -C ${JSON.stringify(worktreePath)} notes show ${sha}`,
      { encoding: "utf8", stdio: "pipe" },
    ).trim()
  } catch {
    return ""
  }
}

// Zwraca listę { note, commit } — commitów, które mają noty w refs/notes/commits.
function gitNotesList(): { note: string; commit: string }[] {
  try {
    const out = execSync(`git -C ${JSON.stringify(worktreePath)} notes list`, { encoding: "utf8", stdio: "pipe" })
    return out
      .split("\n")
      .filter(Boolean)
      .map((l) => {
        const [note, commit] = l.trim().split(/\s+/)
        return { note: note ?? "", commit: commit ?? "" }
      })
      .filter((x) => x.commit)
  } catch {
    return []
  }
}

// commitSha -> { date, subject } dla wszystkich commitów (także na innych gałęziach).
function gitLogMap(): Record<string, { date: string; subject: string }> {
  const map: Record<string, { date: string; subject: string }> = {}
  try {
    const out = execSync(
      `git -C ${JSON.stringify(worktreePath)} log --all --format=%H%x09%ct%x09%s`,
      { encoding: "utf8", stdio: "pipe" },
    )
    for (const l of out.split("\n").filter(Boolean)) {
      const [sha, ct, ...rest] = l.split("\t")
      if (!sha) continue
      map[sha] = {
        date: new Date(Number(ct) * 1000).toISOString(),
        subject: rest.join("\t"),
      }
    }
  } catch {
    // ignore
  }
  return map
}

function gitDiffText(fromSha: string, toSha: string): string {
  try {
    return execSync(
      `git -C ${JSON.stringify(worktreePath)} diff --stat --patch ${fromSha}..${toSha}`,
      { encoding: "utf8", stdio: "pipe" },
    )
  } catch (e: any) {
    return `git diff failed: ${e?.message ?? e}`
  }
}

// --- Feature registry (lokalna definicja feature'ów) --------------------------

type FeatureDef = { name: string; description: string; createdAt: string }

function featuresPath(): string {
  return join(memoryDir, "cache", "features.json")
}

function readFeatures(): Record<string, FeatureDef> {
  return readJson<Record<string, FeatureDef>>(featuresPath()) ?? {}
}

function writeFeatures(features: Record<string, FeatureDef>) {
  writeJson(featuresPath(), features)
}

// Parseuje noty (append kumuluje bloki rozdzielone pustą linią) i zwraca listę
// oznaczeń: { feature, status, sha, date, note, commit }.
function parseFeatureMarks(): {
  feature: string
  status: string
  sha: string
  date: string
  note: string
  commit: string
}[] {
  const logMap = gitLogMap()
  const out: { feature: string; status: string; sha: string; date: string; note: string; commit: string }[] = []
  for (const { commit } of gitNotesList()) {
    const text = gitNotesShow(commit)
    if (!text) continue
    for (const block of text.split(/\n\n+/)) {
      const feature = block.match(/^feature:(\S+)$/m)?.[1]
      if (!feature) continue
      const status = block.match(/^status=(\S+)$/m)?.[1] ?? ""
      const sha = block.match(/^sha=(\S+)$/m)?.[1] ?? ""
      out.push({
        feature,
        status,
        sha,
        date: logMap[commit]?.date ?? "",
        note: block,
        commit,
      })
    }
  }
  return out
}

// Znajdź najnowszy commit (wg daty commita), na którym feature był oznaczony ok.
function lastKnownGoodForFeature(name: string): { commit: string; date: string; sha: string; note: string } | null {
  const marks = parseFeatureMarks().filter((m) => m.feature === name && m.status === "ok")
  if (!marks.length) return null
  let best: (typeof marks)[number] | null = null
  for (const m of marks) {
    if (!best || m.date > best.date) best = m
  }
  if (!best) return null
  return { commit: best.commit, date: best.date, sha: best.sha, note: best.note }
}

// --- /regression feature: definiowanie i weryfikacja feature'ów ---------------

function regressionFeature(args: string): string {
  const parts = args.trim().split(/\s+/).filter(Boolean)
  const sub = parts[0] ?? ""
  const rest = parts.slice(1).join(" ")

  if (sub === "add") {
    const name = parts[1] ?? ""
    if (!name) return "Użycie: /regression feature add <nazwa> [opis]"
    const features = readFeatures()
    if (features[name]) return `Feature "${name}" już istnieje. Użyj /regression feature list.`
    features[name] = { name, description: rest.replace(/^[^ ]+/, "").trim(), createdAt: new Date().toISOString() }
    writeFeatures(features)
    return `Dodano feature "${name}".\nAby oznaczyć go jako działający na HEAD: /regression feature mark ${name}`
  }

  if (sub === "list") {
    const features = readFeatures()
    const entries = Object.values(features)
    if (!entries.length) return "Brak zdefiniowanych feature'ów. Użyj: /regression feature add <nazwa> [opis]"
    const lines = ["=== Zdefiniowane feature'y ==="]
    for (const f of entries) {
      const known = lastKnownGoodForFeature(f.name)
      lines.push(`- ${f.name}${f.description ? ` — ${f.description}` : ""} [${known ? `ok @ ${known.commit.slice(0, 7)}` : "nieoznaczony"}]`)
    }
    return lines.join("\n")
  }

  if (sub === "mark") {
    const name = parts[1] ?? ""
    if (!name) return "Użycie: /regression feature mark <nazwa> [uwaga]"
    const features = readFeatures()
    if (!features[name]) {
      return `Nieznany feature "${name}". Najpierw go zdefiniuj: /regression feature add ${name}`
    }
    const head = getHeadSha()
    if (!head) return "Nie udało się odczytać HEAD."
    const existing = gitNotesShow(head)
    const already = existing.split(/\n\n+/).some((b) => b.includes(`feature:${name}`) && b.includes("status=ok") && b.includes(`sha=${head}`))
    if (already) return `Feature "${name}" jest już oznaczony jako działający na HEAD (${head.slice(0, 7)}).`
    const userNote = rest.replace(/^[^ ]+/, "").trim().replace(/\s+/g, " ")
    const msg = [
      `feature:${name}`,
      `status=ok`,
      `sha=${head}`,
      `date=${new Date().toISOString()}`,
      userNote ? `note=${userNote}` : "",
    ].filter(Boolean).join("\n")
    const res = gitNotesAppend(head, msg)
    return `${res}\nFeature "${name}" oznaczony jako DZIAŁAJĄCY na commicie ${head.slice(0, 7)}.`
  }

  if (sub === "check") {
    const name = parts[1] ?? ""
    if (!name) return "Użycie: /regression feature check <nazwa>"
    const features = readFeatures()
    if (!features[name]) {
      return `Nieznany feature "${name}". Najpierw go zdefiniuj: /regression feature add ${name}`
    }
    const head = getHeadSha()
    const known = lastKnownGoodForFeature(name)
    const lines: string[] = [`=== Feature: ${name} ===`]
    if (features[name].description) lines.push(`Opis: ${features[name].description}`)

    if (!known) {
      lines.push("Ten feature NIGDY nie był oznaczony jako działający w środowisku docelowym.")
      lines.push(`Aby to zrobić po weryfikacji: /regression feature mark ${name}`)
      return lines.join("\n")
    }

    const knownShort = known.commit.slice(0, 7)
    if (known.commit === head) {
      lines.push(`Oznaczony jako DZIAŁAJĄCY w HEAD (${knownShort}, ${known.date.slice(0, 19).replace("T", " ")}).`)
      lines.push("Brak zmian od ostatniej weryfikacji — brak podejrzeń o regresję.")
      return lines.join("\n")
    }

    const logMap = gitLogMap()
    const knownSubject = logMap[known.commit]?.subject ?? "(brak tematu)"
    const commits = gitCommitsBetween(known.commit, head)
    const files = gitFilesChangedBetween(known.commit, head)
    const diff = truncateLines(gitDiffText(known.commit, head), 200)

    lines.push(`Ostatnio oznaczony jako DZIAŁAJĄCY: ${knownShort} (${knownSubject}, ${known.date.slice(0, 19).replace("T", " ")})`)
    lines.push(`Okno zmian: ${knownShort}..HEAD — ${commits.length} commitów, ${files.length} plików`)
    lines.push("")
    lines.push("Commity w oknie:")
    for (const c of commits.slice(0, 20)) lines.push(`  ${c}`)
    lines.push("")
    lines.push("Zmienione pliki:")
    for (const f of files.slice(0, 40)) lines.push(`  ${f}`)
    lines.push("")
    lines.push("=== Prompt dla modelu kodującego ===")
    lines.push(`Mam feature "${name}"${features[name].description ? ` (${features[name].description})` : ""}, który był `)
    lines.push(`oznaczony jako działający poprawnie w commicie ${knownShort} (${knownSubject}, ${known.date}).`)
    lines.push(`Od tego commita do HEAD (${commits.length} commitów, ${files.length} plików) wprowadzono zmiany, `)
    lines.push(`które mogły spowodować regresję. Przeanalizuj TYLKO zmiany poniżej i wskaż, która z nich `)
    lines.push(`mogła złamać feature "${name}" — nie analizuj całego kodu.`)
    lines.push("")
    lines.push("Zmiany (diff):")
    lines.push("```diff")
    lines.push(diff || "(brak diffa)")
    lines.push("```")
    return lines.join("\n")
  }

  return [
    "Użycie: /regression feature <podkomenda>",
    "  add <nazwa> [opis]        — zdefiniuj feature",
    "  list                      — lista zdefiniowanych feature'ów + status",
    "  mark <nazwa> [uwaga]      — oznacz feature jako działający na HEAD (git notes)",
    "  check <nazwa>             — sprawdź czy był oznaczony; pokaż zmiany od znanego-dobrego commita + prompt dla modelu",
  ].join("\n")
}

// Znajdź „last good run" i „first red run" po pierwszym niepustymfailed test name.
// lastGood: ostatni uruchomienie z exit=0 (lub failed puste) przed pierwszym red.
// firstRed: pierwsze uruchomienie z exit!=0 z failed testem, którego wcześniej nie było.
function findRegressionWindow(): {
  lastGood: TestRun | null
  firstRed: TestRun | null
  failingTest: string
} {
  if (testHistory.length < 2) return { lastGood: null, firstRed: null, failingTest: "" }
  // iteruj od najnowszego wstecz, znajdź ostatni zielony
  const reversed = [...testHistory].reverse()
  let lastGreenIdx = -1
  for (let i = 0; i < reversed.length; i++) {
    if (reversed[i].exitCode === 0) { lastGreenIdx = i; break }
  }
  if (lastGreenIdx === -1) {
    // brak zielonego — użyj najstarszego wpisu jako „początek"
    return { lastGood: reversed[reversed.length - 1], firstRed: reversed[0], failingTest: reversed[0]?.failed?.[0] ?? "" }
  }
  const lastGood = reversed[lastGreenIdx]
  // szukaj pierwszego red PO lastGood (czyli w indeksie mniejszym niż lastGreenIdx w reversed)
  let firstRed: TestRun | null = null
  for (let i = lastGreenIdx - 1; i >= 0; i--) {
    if (reversed[i].exitCode !== 0 && reversed[i].failed.length) {
      firstRed = reversed[i]
      break
    }
  }
  return {
    lastGood,
    firstRed,
    failingTest: firstRed?.failed?.[0] ?? "",
  }
}

// Pliki podejrzane = edytowane między lastGood a firstRed, sortowane po liczbie edycji w trace
function suspectFiles(): string[] {
  const { lastGood, firstRed } = findRegressionWindow()
  if (!lastGood && !firstRed) return []
  // 1. pliki zmienione między SHA (jeśli mamy head'y)
  const filesFromGit: string[] = []
  if (lastGood?.head && firstRed?.head && lastGood.head !== firstRed.head) {
    filesFromGit.push(...gitFilesChangedBetween(lastGood.head, firstRed.head))
  } else if (firstRed?.head) {
    // brak lastGood.head — użyj stanu roboczego vs firstRed.head
    filesFromGit.push(...gitFilesChangedBetween("", firstRed.head))
  }
  // 2. pliki z trace.editedFiles, edytowane w oknie czasowym lastGood→firstRed
  const tStart = lastGood ? Date.parse(lastGood.timestamp) : 0
  const tEnd = firstRed ? Date.parse(firstRed.timestamp) : Date.now()
  const traceFiles: string[] = []
  const g = readJson<SessionTrace>(sessionTracePath())
  // trace nie ma timestampów per edycja, więc użyjemy wszystkich edytowanych plików
  // jako priorytetu sortowania, a okno czasowe bierzemy z git/active-session
  if (g?.editedFiles) {
    traceFiles.push(...Object.keys(g.editedFiles))
  }
  // 3. pliki zmodyfikowane obecnie (stan roboczy)
  const dirty = gitStatusPorcelain()
  // scalanie z priorytetem: dirty > git diff range > trace, sortowane po liczbie edycji
  const score: Record<string, number> = {}
  const add = (f: string, w: number) => { score[f] = (score[f] ?? 0) + w }
  for (const f of dirty) add(f, 100)
  for (const f of filesFromGit) add(f, 50)
  for (const f of traceFiles) add(f, (g?.editedFiles[f] ?? 1))
  return Object.entries(score)
    .sort((a, b) => b[1] - a[1])
    .map(([f]) => f)
    .slice(0, 20)
}

// ---------------------------------------------------------------------------
// Regression commands
// ---------------------------------------------------------------------------

function regressionLastGood(): string {
  const { lastGood, firstRed, failingTest } = findRegressionWindow()
  if (!lastGood && !firstRed) {
    return "Brak danych w test-history. Uruchom testy/build, aby plugin zebrał statystyki."
  }
  const lines: string[] = ["=== Regression window ==="]
  if (lastGood) {
    lines.push(`Last good:  ${lastGood.timestamp}  exit=${lastGood.exitCode}  ${lastGood.command}`)
    lines.push(`            HEAD: ${lastGood.head || "(brak SHA)"}`)
    lines.push(`            ${lastGood.summary}`)
  } else {
    lines.push("Last good:  (brak udanego uruchomienia w historii)")
  }
  if (firstRed) {
    lines.push(`First red:  ${firstRed.timestamp}  exit=${firstRed.exitCode}  ${firstRed.command}`)
    lines.push(`            HEAD: ${firstRed.head || "(brak SHA)"}`)
    lines.push(`            ${firstRed.summary}`)
    lines.push(`            failed: ${firstRed.failed.slice(0, 5).join(", ")}`)
  } else {
    lines.push("First red:  (brak nieudanego uruchomienia — brak regresji?)")
  }
  if (failingTest) lines.push(`Failing test: ${failingTest}`)
  return lines.join("\n")
}

function regressionSuspect(): string {
  const { lastGood, firstRed, failingTest } = findRegressionWindow()
  const files = suspectFiles()
  const out: string[] = ["=== Regression suspects ==="]
  if (failingTest) out.push(`Failing test: ${failingTest}`)
  if (lastGood && firstRed) {
    out.push(`Okno: ${lastGood.timestamp} → ${firstRed.timestamp}`)
    if (lastGood.head && firstRed.head && lastGood.head !== firstRed.head) {
      out.push(`Commity w oknie:`)
      for (const c of gitCommitsBetween(lastGood.head, firstRed.head).slice(0, 15)) out.push(`  ${c}`)
    }
  }
  if (!files.length) {
    out.push("Brak podejrzanych plików (brak zmian w oknie lub brak danych).")
  } else {
    out.push(`Pliki zmienione w oknie (posortowane wg prawdopodobieństwa):`)
    for (const f of files) out.push(`  ${f}`)
    out.push("")
    out.push("Podpowiedź: przywróć podejrzany plik do wersji last-good:")
    if (lastGood?.head) out.push(`  /regression revert <plik>      (wymaga /regression revert confirm)`)
    out.push(`  /regression revert stash       (zachowaj wszystkie zmiany w stash)`)
  }
  return out.join("\n")
}

function regressionRevert(args: string): string {
  const safe = cfg.regressionSafeRevertOnly
  const { lastGood } = findRegressionWindow()
  if (!lastGood) return "Brak last-good w historii — nie ma do czego wracać."
  const sha = lastGood.head
  if (!sha) return "Last-good nie ma zapisanego HEAD SHA. Włącz regressionTrackHead, by zbierać SHA."

  const parts = args.trim().split(/\s+/)
  const action = parts[0] ?? ""

  if (action === "stash") {
    const msg = gitStash(`push -m "opencode regression revert @ ${new Date().toISOString()}"`)
    return `Stash: ${msg}\nZmiany zachowane w stash. Sprawdź: git stash list, git stash pop.`
  }

  if (action === "all") {
    const confirmed = parts[1] === "confirm"
    if (safe && !confirmed) {
      return [
        "OSTRZEŻENIE: 'all' wykonuje git checkout <sha> -- . (przywraca WSZYSTKIE pliki do last-good).",
        "Tryb bezpieczny (regressionSafeRevertOnly) wymaga potwierdzenia. Aby wykonać, wpisz:",
        `  /regression revert all confirm`,
      ].join("\n")
    }
    for (const f of suspectFiles()) gitCheckoutFileFromSha(sha, f)
    return `Przywrócono wszystkie podejrzane pliki do wersji ${sha.slice(0, 7)}.`
  }

  if (action === "confirm") {
    // drugi człon to nazwa pliku lub 'all'
    const target = parts[1] ?? ""
    if (target === "all") {
      for (const f of suspectFiles()) gitCheckoutFileFromSha(sha, f)
      return `Przywrócono wszystkie podejrzane pliki do wersji ${sha.slice(0, 7)}.`
    }
    if (target) {
      return gitCheckoutFileFromSha(sha, target)
    }
    return "Użycie: /regression revert confirm <plik|all>"
  }

  // brak akcji — podpowiedź
  if (!action) {
    return [
      "Użycie /regression revert:",
      "  /regression revert <plik>            — przywróć pojedynczy plik do last-good",
      "  /regression revert all               — przywróć wszystkie podejrzane pliki (wymaga confirm w trybie bezpiecznym)",
      "  /regression revert all confirm        — wykonaj przywrócenie wszystkich",
      "  /regression revert stash              — zstashuj wszystkie niezatwierdzone zmiany",
      "",
      `Last-good HEAD: ${sha.slice(0, 7)}`,
      `Podejrzane pliki: ${suspectFiles().slice(0, 5).join(", ") || "(brak)"}`,
    ].join("\n")
  }

  // domyślnie: pojedynczy plik
  if (safe) {
    return [
      `Przywrócenie ${action} do ${sha.slice(0, 7)} wymaga potwierdzenia (tryb bezpieczny).`,
      `Wpisz: /regression revert confirm ${action}`,
    ].join("\n")
  }
  return gitCheckoutFileFromSha(sha, action)
}

// ---------------------------------------------------------------------------
// Context injection on session.created
// ---------------------------------------------------------------------------

function buildInjectedContext(facts: string, session: ActiveSession | null, git: { branch: string; head: string; modified: string[]; recentDiff: string[] }): string {
  const lines: string[] = ["PROJECT MEMORY"]
  if (facts) {
    // extract first meaningful lines (Architecture, Rules, Commands) - keep concise
    const compact = facts
      .split("\n")
      .filter((l) => l.trim().length > 0)
      .slice(0, 25)
      .join("\n")
    lines.push(compact)
  }
  if (git.branch) lines.push(`Git: ${git.branch} @ ${git.head}`)
  if (git.modified.length) lines.push(`Modified files: ${git.modified.slice(0, 10).join(", ")}`)
  if (git.recentDiff.length) lines.push(`Recent changes: ${git.recentDiff.slice(0, 10).join(", ")}`)
  if (session) {
    if (session.goal) lines.push(`Previous task: ${session.goal}`)
    if (session.currentStatus) lines.push(`Status: ${session.currentStatus}`)
    if (session.modifiedFiles?.length) lines.push(`Previously edited: ${session.modifiedFiles.slice(0, 10).join(", ")}`)
    if (session.testStatus) lines.push(`Last test (${session.testStatus.exitCode}): ${session.testStatus.summary}`)
    if (session.blockers?.length) lines.push(`Blockers: ${session.blockers.join("; ")}`)
  }
  let block = lines.join("\n")
  const budget = cfg.maxProjectMemoryTokens
  if (estimateTokens(block) > budget) {
    block = truncateByBytes(block, budget * 4)
    block += "\n[truncated to memory budget]"
  }
  return block
}

// ---------------------------------------------------------------------------
// Handoff building on session.idle / session.compacted
// ---------------------------------------------------------------------------

function buildHandoff(sessionId: string, edits: string[]): ActiveSession {
  const prev = readActiveSession()
  const s: ActiveSession = {
    schemaVersion: 1,
    sessionId,
    updatedAt: new Date().toISOString(),
    goal: prev?.goal ?? "",
    currentStatus: prev?.currentStatus ?? "",
    modifiedFiles: Array.from(new Set([...(prev?.modifiedFiles ?? []), ...edits])).slice(0, 20),
    decisions: prev?.decisions ?? [],
    commands: prev?.commands ?? {},
    testStatus: prev?.testStatus,
    blockers: prev?.blockers ?? [],
    lspErrors: prev?.lspErrors,
  }
  return s
}

// ---------------------------------------------------------------------------
// Tool result filtering
// ---------------------------------------------------------------------------

function saveArtifact(content: string): string {
  const id = shortHash(content)
  const path = join(artifactsDir(), `${id}.log`)
  ensureDir(artifactsDir())
  writeFileSync(path, content, "utf8")
  metrics.artifactsCreated += 1
  metrics.artifactBytes += Buffer.byteLength(content)
  enforceArtifactLimits()
  return id
}

function enforceArtifactLimits() {
  try {
    const dir = artifactsDir()
    const files = readdirSync(dir)
      .map((f) => ({ f, mtime: statSync(join(dir, f)).mtimeMs, size: statSync(join(dir, f)).size }))
      .sort((a, b) => a.mtime - b.mtime)
    const totalMB = files.reduce((acc, f) => acc + f.size, 0) / (1024 * 1024)
    if (totalMB <= MAX_ARTIFACT_DIR_MB) {
      // also prune by TTL
      const now = Date.now()
      for (const f of files) {
        if (now - f.mtime > ARTIFACT_TTL_MS) {
          unlinkSync(join(dir, f.f))
        }
      }
      return
    }
    // remove oldest until under limit
    let removed = 0
    for (const f of files) {
      if (totalMB - removed <= MAX_ARTIFACT_DIR_MB) break
      unlinkSync(join(dir, f.f))
      removed += f.size / (1024 * 1024)
    }
  } catch {
    // ignore
  }
}

function summarizeBuildTest(result: string, exitCode: number, command: string): string {
  const lines = result.split("\n")
  const failed: string[] = []
  const errors: string[] = []
  let summary = ""
  for (const l of lines) {
    if (/FAIL|failed|error|Error|ERROR/.test(l)) {
      if (/FAILED|failed:/i.test(l) || /\berror\b/i.test(l)) {
        if (errors.length < 5) errors.push(l.trim())
        else if (!summary) summary = l.trim()
      }
      failed.push(l.trim())
    }
    if (/passed|PASS|tests? \d|summary|Result:|BUILD/i.test(l)) {
      summary = l.trim()
    }
  }
  const head = lines.slice(0, 5).join("\n")
  const tail = lines.slice(-15).join("\n")
  const parts = [
    `Command: ${command} (exit ${exitCode})`,
    summary ? `Summary: ${summary}` : "",
    errors.length ? `Errors:\n${errors.join("\n")}` : "",
    head ? `Head:\n${head}` : "",
    tail ? `Tail:\n${tail}` : "",
  ].filter(Boolean)
  return parts.join("\n")
}

function summarizeDiff(result: string): string {
  const lines = result.split("\n")
  if (lines.length <= cfg.maxDiffLines) return result
  // files list
  const files = lines
    .filter((l) => l.startsWith("diff --git") || l.startsWith("+++") || l.startsWith("---"))
    .map((l) => {
      if (l.startsWith("diff --git a/")) return l.replace(/^diff --git a\//, "").replace(/\sb\/.*$/, "")
      if (l.startsWith("+++ b/")) return l.slice(6)
      if (l.startsWith("--- a/")) return l.slice(6)
      if (l.startsWith("+++ ")) return l.slice(4)
      if (l.startsWith("--- ")) return l.slice(4)
      return l
    })
    .slice(0, 20)
  // first hunk(s)
  const hunks: string[] = []
  let cur: string[] = []
  let inHunk = false
  for (const l of lines) {
    if (l.startsWith("@@")) {
      if (cur.length) hunks.push(cur.join("\n"))
      cur = [l]
      inHunk = true
    } else if (inHunk) {
      cur.push(l)
    }
  }
  if (cur.length) hunks.push(cur.join("\n"))
  const selected = hunks.slice(0, 3).join("\n")
  const id = saveArtifact(result)
  const summary = `Changed files:\n${files.join("\n")}\n\nTop hunk(s):\n${truncateLines(selected, cfg.maxDiffLines - files.length - 5)}\n\nFull diff available: artifact://${id}`
  return summary
}

function summarizeSearch(result: string): string {
  const lines = result.split("\n")
  const matches: { file: string; line: string; ctx: string }[] = []
  let currentFile = ""
  for (const l of lines) {
    const m = l.match(/^(.+?):(\d+):\s*(.*)$/)
    if (m) {
      currentFile = m[1]
      matches.push({ file: m[1], line: m[2], ctx: m[3] })
    }
  }
  // dedupe identical
  const seen = new Set<string>()
  const unique = matches.filter((m) => {
    const k = `${m.file}:${m.ctx}`
    if (seen.has(k)) return false
    seen.add(k)
    return true
  })
  // group by file
  const grouped: Record<string, string[]> = {}
  for (const m of unique) {
    if (!grouped[m.file]) grouped[m.file] = []
    grouped[m.file].push(`${m.line}: ${m.ctx}`)
  }
  const files = Object.keys(grouped).slice(0, 30)
  let out = ""
  let count = 0
  for (const f of files) {
    if (count >= cfg.maxSearchMatches) break
    out += `${f}\n`
    for (const l of grouped[f]) {
      if (count >= cfg.maxSearchMatches) break
      out += `  ${l}\n`
      count++
    }
  }
  if (unique.length > count) {
    const id = saveArtifact(result)
    out += `\n... ${unique.length - count} more matches. Full output: artifact://${id}`
  }
  return out || result
}

function dedupeRead(filePath: string, content: string, lineStart?: number, lineEnd?: number): string | null {
  if (!cfg.deduplicateReadResults) return content
  const h = hashContent(content)
  const key = dedupKey({ filePath, contentHash: h, lineStart, lineEnd })
  const existing = seen.get(key)
  if (existing) {
    metrics.deduplicatedReads += 1
    metrics.dedupSavedChars += content.length
    return `Already delivered in this session (hash: ${h.slice(0, 8)}, source: ${existing.source}, at ${existing.deliveredAt}). Use read_artifact or read again explicitly if needed.`
  }
  seen.set(key, { filePath, contentHash: h, lineStart, lineEnd, deliveredAt: new Date().toISOString(), source: "read" })
  return content
}

function filterToolResult(tool: string, args: any, result: string, exitCode = 0): string {
  if (typeof result !== "string") return result
  let filtered = result
  metrics.rawChars += result.length

  if (tool === "bash") {
    const cmd: string = args?.command ?? ""
    if (/git diff/.test(cmd)) {
      filtered = summarizeDiff(result)
    } else if (isBuildTestCommand(cmd)) {
      filtered = summarizeBuildTest(result, exitCode, cmd)
      // --- Addition 3: record test run in history ---
      failOpen(() => {
        const normalized = normalizeBuildTestCommand(cmd)
        const run: TestRun = {
          timestamp: new Date().toISOString(),
          command: cmd.slice(0, 200),
          exitCode,
          summary: parseTestSummary(result),
          failed: parseFailedTests(result, cmd),
          sessionId: lastSessionId,
          head: cfg.regressionTrackHead ? getHeadSha() : "",
        }
        recordTestRun(run)
        // --- Addition 2: track in session trace ---
        sessionTrace.buildTestCommands[normalized] = (sessionTrace.buildTestCommands[normalized] ?? 0) + 1
        if (exitCode !== 0 && run.failed.length) {
          for (const f of run.failed.slice(0, 5)) {
            if (!sessionTrace.blockers.includes(f)) sessionTrace.blockers.push(f)
          }
        }
        saveSessionTrace()
      }, "recordTestRun")
    } else if (/grep|rg /.test(cmd)) {
      filtered = summarizeSearch(result)
    }
  } else if (tool === "grep") {
    filtered = summarizeSearch(result)
  } else if (tool === "read") {
    const fp: string = args?.filePath ?? ""
    const ls = args?.offset
    const le = args?.limit ? (args.offset ?? 0) + args.limit : undefined
    const deduped = dedupeRead(fp, result, ls, le)
    if (deduped !== result) filtered = deduped ?? result
  }

  // mask secrets
  filtered = maskSecrets(filtered)

  // global line limit
  if (filtered.split("\n").length > cfg.maxToolResultLines) {
    if (cfg.storeFullArtifacts && filtered === result) {
      const id = saveArtifact(result)
      filtered = truncateLines(filtered, cfg.maxToolResultLines) + `\n\nFull output available: artifact://${id}`
    } else {
      filtered = truncateLines(filtered, cfg.maxToolResultLines)
    }
  }

  metrics.deliveredChars += filtered.length
  metrics.toolCalls += 1
  return filtered
}

// ---------------------------------------------------------------------------
// Custom tool: read_artifact
// ---------------------------------------------------------------------------

function readArtifact(artifactId: string, offset = 0, limit = cfg.maxArtifactPreviewLines, search?: string): string {
  const path = join(artifactsDir(), `${artifactId}.log`)
  if (!existsSync(path)) return `Artifact ${artifactId} not found.`
  let content = readText(path)
  if (search) {
    const lines = content.split("\n")
    const matched = lines.filter((l) => l.includes(search))
    content = matched.join("\n")
  }
  const lines = content.split("\n")
  const slice = lines.slice(offset, offset + limit)
  return slice.join("\n") + `\n\n[artifact ${artifactId}: lines ${offset}-${offset + slice.length} of ${lines.length}]`
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

function dirSizeBytes(dir: string): number {
  let total = 0
  let entries: string[] = []
  try { entries = readdirSync(dir) } catch { return 0 }
  for (const f of entries) {
    const full = join(dir, f)
    let st
    try { st = statSync(full) } catch { continue }
    if (st.isDirectory()) total += dirSizeBytes(full)
    else total += st.size
  }
  return total
}

const FLUSH_METRICS_MIN_INTERVAL_MS = 2000
let lastFlushMetricsAt = 0

function flushMetrics() {
  // Lekkie metryki: liczniki, estymaty, AI — bez git/stat/IO poza writeJson.
  // Ciężkie (git, stat, regression) aktualizowane tylko przez flushMetricsHeavy()
  // na sesja.created/idle/compacted — nie przy każdym tool callu.
  const now = Date.now()
  if (now - lastFlushMetricsAt < FLUSH_METRICS_MIN_INTERVAL_MS) return
  lastFlushMetricsAt = now
  metrics.estimatedReductionPercent = metrics.rawChars > 0
    ? +(((metrics.rawChars - metrics.deliveredChars) / metrics.rawChars) * 100).toFixed(1)
    : 0
  metrics.estimatedSavedChars = Math.max(0, metrics.rawChars - metrics.deliveredChars) + metrics.dedupSavedChars
  metrics.estimatedSavedTokens = Math.round(metrics.estimatedSavedChars / 4)
  metrics.contextTokens = lastContextTokens
  metrics.contextLimit = effectiveContextLimit()
  metrics.compactThresholdPct = cfg.compactThreshold
  metrics.compactMode = cfg.compactMode
  metrics.diskLimitBytes = MAX_ARTIFACT_DIR_MB * 1024 * 1024
  metrics.dedupCacheMax = cfg.maxDedupCacheEntries
  metrics.dedupCacheCount = seen.size
  metrics.testHistoryMax = cfg.maxTestHistoryEntries
  // --- AI live stats (lekkie, tylko kopiowanie z aiStatus) ---
  metrics.aiEnabled = aiStatus.enabled
  metrics.aiProvider = aiStatus.provider
  metrics.aiModel = aiStatus.model
  metrics.aiCalls = aiStatus.calls
  metrics.aiSuccesses = aiStatus.successes
  metrics.aiFailures = aiStatus.failures
  metrics.aiLastCallMs = aiStatus.lastCallMs
  metrics.aiLastError = aiStatus.lastError
  metrics.aiHealthState = aiStatus.healthState
  metrics.aiBusy = aiStatus.busy
  metrics.aiBusyLabel = aiStatus.busyLabel
  metrics.aiBusySince = aiStatus.busySince
  metrics.aiConfigTimeoutMs = aiStatus.configTimeoutMs
  metrics.aiMaxObservedMs = aiStatus.maxObservedMs
  metrics.aiLastDurationMs = aiStatus.lastDurationMs
  metrics.aiTimeoutWarn = aiStatus.timeoutWarn
  metrics.aiTimeoutExtended = aiStatus.timeoutExtended
  writeJson(metricsPath(), metrics)
  // Addition 1: persist dedup cache to disk
  failOpen(() => saveDedupCache(), "saveDedupCache")
}

function flushMetricsHeavy(dirtyCount?: number) {
  // Ciężkie metryki: git, stat, regression — wołane tylko na eventach sesji.
  // dirtyCount opcjonalnie z zewnątrz (gdy session.idle już policzyło gitStatusPorcelain).
  metrics.headSha = failOpenReturn(() => getHeadSha().slice(0, 8), "", "flushMetrics headSha")
  metrics.dirtyFiles = dirtyCount ?? failOpenReturn(() => gitStatusPorcelain().length, 0, "flushMetrics dirty")
  metrics.artifactsBytes = dirSizeBytes(artifactsDir())
  metrics.cacheBytes = dirSizeBytes(join(memoryDir, "cache"))
  metrics.diskBytes = metrics.artifactsBytes + metrics.cacheBytes
  metrics.factsMaxTokens = cfg.maxProjectMemoryTokens
  metrics.factsTokens = failOpenReturn(() => estimateTokens(readText(factsPath()) ?? ""), 0, "flushMetrics factsTokens")
  metrics.testHistoryCount = testHistory.length
  const sess = failOpenReturn(() => readActiveSession(), null, "flushMetrics activeSession")
  if (sess) {
    metrics.modifiedCount = sess.modifiedFiles?.length ?? 0
    metrics.decisionsCount = sess.decisions?.length ?? 0
    metrics.blockersCount = sess.blockers?.length ?? 0
    metrics.lspErrorsCount = sess.lspErrors?.length ?? 0
    if (sess.updatedAt) {
      const ageMs = Date.now() - Date.parse(sess.updatedAt)
      metrics.handoffAgeMin = ageMs > 0 ? Math.floor(ageMs / 60000) : 0
    } else {
      metrics.handoffAgeMin = 0
    }
  } else {
    metrics.modifiedCount = 0
    metrics.decisionsCount = 0
    metrics.blockersCount = 0
    metrics.lspErrorsCount = 0
    metrics.handoffAgeMin = 0
  }
  const reg = failOpenReturn(() => findRegressionWindow(), { lastGood: null, firstRed: null, failingTest: "" }, "flushMetrics regression")
  metrics.lastGoodHead = reg.lastGood?.head?.slice(0, 8) ?? ""
  // Artifact list for TUI (avoids separate readdirSync+statSync every 3s)
  metrics.artifactsList = failOpenReturn(() => {
    const dir = artifactsDir()
    const files = readdirSync(dir).filter((f) => f.endsWith(".log"))
    return files.map((f) => {
      let bytes = 0
      try { bytes = statSync(join(dir, f)).size } catch {}
      return { id: f.replace(/\.log$/, ""), bytes }
    }).sort((a, b) => b.bytes - a.bytes)
  }, [], "flushMetrics artifactsList")
  writeJson(metricsPath(), metrics)
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

function memoryStatus(): string {
  const facts = readText(factsPath())
  const sess = readActiveSession()
  let artifacts = 0
  let artifactBytes = 0
  try {
    const dir = artifactsDir()
    for (const f of readdirSync(dir)) {
      const st = statSync(join(dir, f))
      artifacts += 1
      artifactBytes += st.size
    }
  } catch {
    // ignore
  }
  return [
    `Worktree: ${worktreePath}`,
    `Project facts: ${estimateTokens(facts)} tokens (${facts.length} chars)`,
    `Active session: ${sess?.sessionId ?? "none"} (updated ${sess?.updatedAt ?? "-"})`,
    `Dedup cache: ${seen.size} wpisów${cfg.persistentDedupCache ? " (trwały na dysku)" : " (tylko RAM)"}`,
    `Artifacts: ${artifacts} (${(artifactBytes / 1024).toFixed(1)} KB)`,
    `Test history: ${testHistory.length} uruchomień`,
    `Metrics: ${metrics.toolCalls} tool calls, ${metrics.estimatedReductionPercent}% reduction, ${metrics.deduplicatedReads} dedup reads`,
    `Saved: ~${metrics.estimatedSavedTokens.toLocaleString()} tokens (${metrics.estimatedSavedChars.toLocaleString()} chars) — filtracja + deduplikacja`,
    `Context: ${lastContextTokens.toLocaleString()} / ${effectiveContextLimit().toLocaleString()} tokens (mode=${cfg.compactMode})`,
  ].join("\n")
}

function fmtBytes(n: number): string {
  if (n >= 1_048_576) return `${(n / 1_048_576).toFixed(1)}MB`
  if (n >= 1024) return `${(n / 1024).toFixed(1)}KB`
  return `${n}B`
}

function fmtTokensShort(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`
  if (n >= 1_000) return `${(n / 1_000).toFixed(1)}k`
  return `${n}`
}

function ageLabel(min: number): string {
  if (min <= 0) return "now"
  if (min < 60) return `${min}m`
  if (min < 1440) return `${Math.floor(min / 60)}h ${min % 60}m`
  return `${Math.floor(min / 1440)}d`
}

// Tekstowy odpowiednik widoku TUI — działa w trybie CLI (non-interactive)
function memoryTuiDump(): string {
  flushMetrics()
  const m = metrics
  const limit = m.contextLimit || effectiveContextLimit()
  const ctxPct = limit > 0 ? Math.round((m.contextTokens / limit) * 100) : 0
  const threshold = m.compactThresholdPct
  const factsPct = m.factsMaxTokens > 0 ? Math.round((m.factsTokens / m.factsMaxTokens) * 100) : 0
  const diskPct = m.diskLimitBytes > 0 ? Math.round((m.diskBytes / m.diskLimitBytes) * 100) : 0
  const dirtyLabel = m.dirtyFiles > 0 ? ` (dirty:${m.dirtyFiles})` : ""
  const lspLabel = m.lspErrorsCount > 0 ? ` · lsp:${m.lspErrorsCount}err` : ""
  const lastGoodLabel = m.lastGoodHead ? ` · last-good:${m.lastGoodHead}` : ""
  const revertsLabel = m.revertsCount > 0 ? ` · reverts:${m.revertsCount}` : ""
  const dedupLabel = m.deduplicatedReads > 0 ? ` · dedup:${m.deduplicatedReads}` : ""
  const artLabel = m.artifactsCreated > 0 ? ` · art:${m.artifactsCreated} (${fmtBytes(m.artifactsBytes)})` : ""

  const warn = (cond: boolean) => cond ? " ⚠" : ""

  return [
    `memory: tools:${m.toolCalls} · saved:~${fmtTokensShort(m.estimatedSavedTokens)} tok · ${m.estimatedReductionPercent}% reduc.${dedupLabel}${artLabel}`,
    `disk: ${fmtBytes(m.diskBytes)} / ${fmtBytes(m.diskLimitBytes)} (${diskPct}%) · art ${fmtBytes(m.artifactsBytes)} · cache ${fmtBytes(m.cacheBytes)}${warn(diskPct >= 90)}`,
    `ctx: ${fmtTokensShort(m.contextTokens)}/${fmtTokensShort(limit)} tok (${ctxPct}%, compact@${threshold}%) · facts: ${fmtTokensShort(m.factsTokens)}/${fmtTokensShort(m.factsMaxTokens)} (${factsPct}%) [${m.compactMode}]${warn(ctxPct >= threshold)}`,
    `handoff: ${ageLabel(m.handoffAgeMin)} · mod:${m.modifiedCount} · dec:${m.decisionsCount} · blk:${m.blockersCount} · HEAD:${m.headSha || "-"}${dirtyLabel}${lspLabel}${lastGoodLabel}${revertsLabel}`,
    `dedup cache: ${m.dedupCacheCount}/${m.dedupCacheMax} · tests: ${m.testHistoryCount}/${m.testHistoryMax}`,
  ].join("\n")
}

function memoryShow(): string {
  const facts = readProjectFacts()
  const sess = readActiveSession()
  return [
    "=== PROJECT FACTS ===",
    facts || "(empty)",
    "",
    "=== ACTIVE SESSION ===",
    sess ? JSON.stringify(sess, null, 2) : "(none)",
    "",
    "=== INJECTED CONTEXT (last session) ===",
    lastInjectedContext || "(none)",
    "",
    "=== TEST HISTORY (ostatnie 5) ===",
    testHistory.length ? testHistory.slice(-5).map((t) => `[${t.timestamp}] exit=${t.exitCode} ${t.command}\n    ${t.summary}`).join("\n") : "(brak)",
    "",
    "=== SESSION TRACE (aggregated) ===",
    (() => {
      const g = readJson<SessionTrace>(sessionTracePath())
      if (!g) return "(brak)"
      return JSON.stringify(g, null, 2)
    })(),
  ].join("\n")
}

function memoryClearSession(): string {
  seen = new Map()
  metrics = {
    ...metrics,
    sessionId: lastSessionId,
    toolCalls: 0,
    rawChars: 0,
    deliveredChars: 0,
    deduplicatedReads: 0,
    dedupSavedChars: 0,
    estimatedReductionPercent: 0,
    estimatedSavedChars: 0,
    estimatedSavedTokens: 0,
    artifactsCreated: 0,
    artifactBytes: 0,
    contextTokens: 0,
    contextLimit: effectiveContextLimit(),
    headSha: "",
    dirtyFiles: 0,
    diskBytes: 0,
    artifactsBytes: 0,
    artifactsList: [],
    cacheBytes: 0,
    handoffAgeMin: 0,
    modifiedCount: 0,
    decisionsCount: 0,
    blockersCount: 0,
    dedupCacheCount: 0,
    testHistoryCount: 0,
    lspErrorsCount: 0,
    lastGoodHead: "",
    revertsCount: 0,
    factsTokens: 0,
  }
  testHistory = []
  sessionTrace = { sessionId: lastSessionId, buildTestCommands: {}, editedFiles: {}, blockers: [], startedAt: new Date().toISOString() }
  try {
    rmSync(activeSessionPath(), { force: true })
    // Preserve aggregated session-trace.json (persistent across sessions).
    // Remove only per-session caches: dedup-seen, test-history, metrics.
    rmSync(dedupCachePath(), { force: true })
    rmSync(testHistoryPath(), { force: true })
    rmSync(metricsPath(), { force: true })
    rmSync(aiThrottlePath(), { force: true })
    rmSync(aiMaxObservedPath(), { force: true })
    aiClearTimeoutOverride()
    // Prune artifacts dir (session-scoped); recreate empty.
    rmSync(artifactsDir(), { recursive: true, force: true })
    ensureDir(artifactsDir())
  } catch {
    // ignore
  }
  return "Session memory cleared (project-facts.md i aggregated trace zachowane)."
}

function memoryClearProject(): string {
  try {
    rmSync(memoryDir, { recursive: true, force: true })
    initMemoryLayout(worktreePath)
  } catch {
    // ignore
  }
  return "All project memory cleared."
}

function contextBudget(): string {
  const facts = readProjectFacts()
  const sess = readActiveSession()
  const handoffTokens = sess ? estimateTokens(JSON.stringify(sess)) : 0
  const limit = effectiveContextLimit()
  const pct = limit > 0 ? Math.round((lastContextTokens / limit) * 100) : 0
  return [
    `Project facts:  ${estimateTokens(facts)} / ${cfg.maxProjectMemoryTokens} tokens`,
    `Handoff:        ${handoffTokens} / ${cfg.maxSessionHandoffTokens} tokens`,
    `Tool result limit: ${cfg.maxToolResultLines} lines`,
    `Diff limit:     ${cfg.maxDiffLines} lines`,
    `Search matches: ${cfg.maxSearchMatches}`,
    `Artifact preview: ${cfg.maxArtifactPreviewLines} lines`,
    `Context size:   ${lastContextTokens.toLocaleString()} / ${limit.toLocaleString()} tokens (${pct}%, mode=${cfg.compactMode}, threshold=${cfg.compactThreshold}%)`,
  ].join("\n")
}

function contextArtifacts(): string {
  try {
    const dir = artifactsDir()
    const files = readdirSync(dir).map((f) => ({ f, st: statSync(join(dir, f)) }))
      .sort((a, b) => b.st.mtimeMs - a.st.mtimeMs)
      .slice(0, 20)
    if (!files.length) return "No artifacts stored."
    return files.map((f) => `${f.f}  ${(f.st.size / 1024).toFixed(1)} KB  ${new Date(f.st.mtimeMs).toISOString()}`).join("\n")
  } catch {
    return "No artifacts stored."
  }
}

// --- Additions: /memory propose | commit | test-history ----------------------

function memoryPropose(): string {
  const proposed = buildProposedFacts()
  writeFileSync(proposedFactsPath(), proposed, "utf8")
  return proposed + "\n\n---\nAby dopisać te propozycje do project-facts.md, uruchom:  /codemem commit"
}

function memoryAutoRefresh(): string {
  refreshAutoFacts()
  const auto = readAutoFacts()
  return auto + `\n\n---\nZregenerowano ${factsAutoPath()}. Wstrzykiwane razem z project-facts.md.`
}

function memoryAutoShow(): string {
  const auto = readAutoFacts()
  if (!auto) return "Auto-fakty wyłączone lub puste. (autoExtractFacts=" + cfg.autoExtractFacts + ")"
  return auto
}

// --- Context compaction -------------------------------------------------------
// Detekcja rozmiaru kontekstu i sugestie kompaktacji. OpenCode ma natywną
// compaction.auto, ale plugin może: (a) podać precyzyjny próg przez autodetekcję
// limitu modelu, (b) sugerować kompaktację toastem, (c) wzbogacić kontekst
// po kompaktacji przez experimental.session.compacting.

const FALLBACK_CONTEXT_LIMIT = 200000

function effectiveContextLimit(): number {
  if (cfg.maxContextTokens > 0) return cfg.maxContextTokens
  if (modelContextLimit > 0) return modelContextLimit
  return FALLBACK_CONTEXT_LIMIT
}

function compactThresholdTokens(): number {
  return Math.floor(effectiveContextLimit() * (cfg.compactThreshold / 100))
}

function compactStatusText(): string {
  const limit = effectiveContextLimit()
  const threshold = compactThresholdTokens()
  const pct = limit > 0 ? Math.round((lastContextTokens / limit) * 100) : 0
  const remaining = Math.max(0, limit - lastContextTokens)
  const lines = [
    "=== Context compaction ===",
    `Tryb:           ${cfg.compactMode}`,
    `Rozmiar:        ${lastContextTokens.toLocaleString()} tokens`,
    `Limit:          ${limit.toLocaleString()} tokens${cfg.maxContextTokens > 0 ? " (ręczny)" : modelContextLimit > 0 ? ` (autodetekcja: ${lastAssistantModel || "?"})` : " (fallback 200k)"}`,
    `Próg kompaktacji: ${threshold.toLocaleString()} tokens (${cfg.compactThreshold}%)`,
    `Wykorzystanie:  ${pct}%`,
    `Pozostało:      ${remaining.toLocaleString()} tokens`,
    `Sugestia pokazana: ${compactSuggestionShown ? "tak" : "nie"}`,
  ]
  if (lastContextTokens >= threshold) {
    lines.push("")
    lines.push("⚠ Próg przekroczony — sugerowana kompaktacja.")
    if (cfg.compactMode === "suggest") lines.push("Aby skompaktować: użyj natywnej komendy OpenCode (np. /compact w TUI) lub /codemem compact-now.")
    else if (cfg.compactMode === "confirm") lines.push("Aby skompaktować: /codemem compact-now. Aby odłożyć: /codemem compact-reset.")
    else if (cfg.compactMode === "auto") lines.push("Tryb auto: OpenCode skompaktuje automatycznie (compaction.auto=true).")
    else if (cfg.compactMode === "off") lines.push("Tryb off: kompaktacja wyłączona w pluginie.")
  }
  return lines.join("\n")
}

async function autodetectModelLimit(client: any, modelKey: string): Promise<number> {
  if (!client || !modelKey) return 0
  const [providerID, modelID] = parseModelKey(modelKey)
  if (!providerID || !modelID) return 0
  try {
    const res = await client?.config?.providers?.()
    const body = res?.body ?? res?.data ?? res
    const providers = body?.providers ?? body?.data?.providers ?? []
    for (const p of providers) {
      if (p.id !== providerID) continue
      const m = p?.models?.[modelID]
      const ctx = m?.limit?.context ?? m?.capabilities?.limit?.context
      if (ctx && ctx > 0) return ctx
    }
  } catch {
    // ignore — fallback
  }
  return 0
}

function maybeShowCompactSuggestion(client: any): void {
  if (cfg.compactMode === "off") return
  const limit = effectiveContextLimit()
  const threshold = compactThresholdTokens()
  if (lastContextTokens < threshold) {
    compactSuggestionShown = false
    return
  }
  if (compactSuggestionShown) return
  compactSuggestionShown = true
  const pct = limit > 0 ? Math.round((lastContextTokens / limit) * 100) : 0
  const msg = `Kontekst ${lastContextTokens.toLocaleString()}/${limit.toLocaleString()} tokens (${pct}%) — powyżej progu ${cfg.compactThreshold}%.`
  if (cfg.compactMode === "suggest" || cfg.compactMode === "confirm") {
    try {
      client?.tui?.showToast?.({ body: { title: "Kompaktacja sugerowana", message: msg, variant: "warning", duration: 8000 } })
    } catch { /* ignore */ }
  }
  // tryb auto: nic nie robimy — OpenCode ma compaction.auto=true i skompaktuje sam
}

async function updateContextTokensFromMessage(event: any, client: any): Promise<void> {
  const info = event?.properties?.info ?? event?.info
  if (!info || info.role !== "assistant") return
  const tokens = info.tokens
  if (!tokens || typeof tokens.input !== "number") return
  lastContextTokens = tokens.input
  const modelKey = info.providerID && info.modelID ? `${info.providerID}/${info.modelID}` : ""
  if (modelKey && modelKey !== lastAssistantModel) {
    lastAssistantModel = modelKey
    if (cfg.maxContextTokens === 0 && modelContextLimit === 0) {
      const lim = await autodetectModelLimit(client, modelKey)
      if (lim > 0) modelContextLimit = lim
    }
  }
  maybeShowCompactSuggestion(client)
}

function memoryCompactStatus(): string {
  return compactStatusText()
}

function memoryCompactReset(): string {
  compactSuggestionShown = false
  return "Flaga sugestii kompaktacji zresetowana. /codemem compact-status pokaże bieżący stan."
}

async function memoryCompactNow(client: any): Promise<string> {
  // Wyzwolenie kompaktacji przez tui.executeCommand z komendą "session.compact"
  // (wartość z enum EventTuiCommandExecute.properties.command, a nie "/compact").
  try {
    if (client?.tui?.executeCommand) {
      await client.tui.executeCommand({ body: { command: "session.compact" } })
      return "Wysłano komendę session.compact do TUI. Jeśli nie zadziałał, użyj natywnej komendy OpenCode."
    }
  } catch { /* ignore */ }
  return [
    "Brak API do programowego wymuszenia kompaktacji.",
    "Opcje:",
    "  1. Wciśnij natywną komendę kompaktacji w OpenCode TUI",
    "  2. Ustaw compaction.auto=true w opencode.json (auto-kompaktacja przy overflow)",
    "  3. Kontynuuj — OpenCode skompaktuje przy następnym message (gdy auto=true)",
    "",
    compactStatusText(),
  ].join("\n")
}

// --- /codemem exit ------------------------------------------------------------
// Wrapper na /exit: przed wyjściem generuje podsumowanie AI sesji, zapisuje je
// do active-session.json (currentStatus) i wywołuje natywną komendę exit TUI.
// AI jest wywoływane synchronicznie (await) — throttle z session.idle NIE ma tu
// zastosowania, bo to żądanie na żądanie użytkownika (jak /codemem ai triage).
// Fallback gdy AI wyłączone/niedostępne: deterministic podsumowanie z handoffa.

async function codememExit(client: any): Promise<string> {
  const edits = gitStatusPorcelain()

  // Odśwież auto-fakty przed zapisem kontekstu (pakiet, testy, struktura).
  if (cfg.autoExtractFacts) failOpen(() => refreshAutoFacts(), "refreshAutoFacts on /codemem exit")

  const handoff = buildHandoff(lastSessionId, edits)
  writeActiveSession(handoff)

  const lines: string[] = []
  let aiError = false

  // AI summary — omija throttle (komenda na żądanie), await przed exit.
  // Generuj zawsze gdy AI włączone i jest JAKAKOLWIEK aktywność sesji,
  // nie tylko git-zmiany (decyzje, blokery, testy też są istotne).
  const aiOn = aiEffectiveConfig() !== null
  const hasActivity = edits.length > 0
    || !!handoff.testStatus
    || handoff.decisions.length > 0
    || handoff.blockers.length > 0
    || handoff.modifiedFiles.length > 0
  if (aiOn && hasActivity) {
    const ok = await failOpenAsync(async () => {
      const summary = await aiSummarizeSession(edits, handoff.testStatus ? {
        command: handoff.testStatus.lastCommand,
        exitCode: handoff.testStatus.exitCode,
        summary: handoff.testStatus.summary,
      } : undefined, {
        decisions: handoff.decisions.slice(0, 5),
        blockers: handoff.blockers.slice(0, 3),
      })
      if (summary) {
        const s = readActiveSession()
        if (s) {
          s.currentStatus = summary.slice(0, 400)
          writeActiveSession(s)
          lines.push(`AI: ${summary}`)
        }
      }
    }, "aiSummarizeSession on /codemem exit")
    if (!ok) aiError = true
  }

  // Deterministic fallback summary gdy AI nie dało wyniku.
  if (lines.length === 0) {
    const bits: string[] = []
    if (handoff.modifiedFiles.length) bits.push(`zmodyfikowano ${handoff.modifiedFiles.length} plik(ów)`)
    if (handoff.testStatus) bits.push(`test ${handoff.testStatus.lastCommand} exit=${handoff.testStatus.exitCode}`)
    if (handoff.blockers.length) bits.push(`${handoff.blockers.length} bloker(ów)`)
    if (handoff.decisions.length) bits.push(`${handoff.decisions.length} decyzji`)
    lines.push(bits.length ? `Sesja: ${bits.join(", ")}.` : "Sesja: brak istotnych zmian.")
  }

  // Wywołaj natywny exit przez TUI, tylko gdy nie było błędu AI.
  // Komenda "exit" odpowiada /exit (alias /quit, /q).
  if (!aiError) {
    let exited = false
    try {
      if (client?.tui?.executeCommand) {
        await client.tui.executeCommand({ body: { command: "exit" } })
        exited = true
      }
    } catch { /* ignore — pozwól zwrócić instrukcję */ }

    if (!exited) {
      lines.push("", "Nie udało się programowo wywołać exit. Naciśnij ctrl+x q lub /exit ręcznie.")
    }
  } else {
    lines.push("", "AI summary nie powiodło się — exit wstrzymany. Sprawdź /codemem ai status.")
  }

  return lines.join("\n")
}

// --- /codemem init ------------------------------------------------------------
// Wykrywa dane z repo (ekstraktory auto-fakts) i wstępnie wypełnia project-facts.md
// podpowiedziami. Idempotentny: nie nadpisuje nietrywialnego pliku; --force nadpisuje.

function isDefaultFactsTemplate(text: string): boolean {
  if (!text) return true
  const head = text.split("\n").slice(0, 6).join("\n")
  return /# Architektura/.test(head) && /\(uzupełnij/.test(head)
}

function buildInitFactsTemplate(force: boolean): { wrote: boolean; path: string; body: string; skipped: string } {
  const root = worktreePath || projectRoot || process.cwd()
  const cmds = extractBuildAndTestCommands(root)
  const env = extractEnvironment(root)
  const arch = extractArchitecture(root)
  const out: string[] = []
  out.push("# project-facts.md")
  out.push("# Fakty ręczne o projekcie. Inicjalizowane przez /codemem init.")
  out.push("# Auto-wykryte wartości to podpowiedzi — edytuj swobodnie. Regenerowane auto-fakty: project-facts.auto.md")
  out.push("")

  // Architektura
  out.push("# Architektura")
  if (arch.stack.length) {
    for (const s of arch.stack) out.push(`- ${s}`)
  } else {
    out.push("- (uzupełnij: stack, główne katalogi, warstwy)")
  }
  if (arch.dirs.length) {
    out.push(`- Główne katalogi: ${arch.dirs.slice(0, 15).join(", ")}`)
  } else {
    out.push("- (uzupełnij: główne katalogi)")
  }
  out.push("")

  // Konwencje (puste — do ręcznego wypełnienia)
  out.push("# Konwencje")
  out.push("- (uzupełnij: reguły kodowania, styl, nazewnictwo)")
  out.push("- (uzupełnij: struktura modułów, warstwy)")
  out.push("")

  // Komendy
  out.push("# Komendy")
  const haveCmd = cmds.build.length || cmds.test.length || cmds.format.length || cmds.lint.length
  if (cmds.build.length) {
    out.push("- Build:")
    for (const c of cmds.build) out.push(`  - ${c}`)
  } else {
    out.push("- Build: (uzupełnij)")
  }
  if (cmds.test.length) {
    out.push("- Testy:")
    for (const c of cmds.test) out.push(`  - ${c}`)
  } else {
    out.push("- Testy: (uzupełnij)")
  }
  if (cmds.format.length) {
    out.push("- Formatowanie:")
    for (const c of cmds.format) out.push(`  - ${c}`)
  }
  if (cmds.lint.length) {
    out.push("- Lint:")
    for (const c of cmds.lint) out.push(`  - ${c}`)
  }
  if (!haveCmd) out.push("- (brak wykrytych manifestów — uzupełnij ręcznie)")
  out.push("")

  // Środowisko
  if (env.length) {
    out.push("# Środowisko")
    for (const e of env) out.push(`- ${e}`)
    out.push("")
  } else {
    out.push("# Środowisko")
    out.push("- (uzupełnij: wersje runtime, kontener, zależności systemowe)")
    out.push("")
  }

  // Ryzyka (puste — do ręcznego wypełnienia)
  out.push("# Ryzyka i znane problemy")
  out.push("- (uzupełnij)")

  const body = out.join("\n") + "\n"
  const path = factsPath()
  const existing = readText(path)

  // Idempotencja: nie nadpisuj, chyba że force lub domyślny szablon
  if (existing && !force && !isDefaultFactsTemplate(existing)) {
    return { wrote: false, path, body, skipped: "istniejący project-facts.md nietrywialny — użyj /codemem init --force, aby nadpisać" }
  }
  if (existing && !force && isDefaultFactsTemplate(existing)) {
    // backup domyślnego szablonu
    try { writeFileSync(path + ".tpl.bak", existing, "utf8") } catch { /* ignore */ }
  }
  writeFileSync(path, body, "utf8")
  return { wrote: true, path, body, skipped: "" }
}

function memoryInit(args: string): string {
  const force = /--force/.test(args)
  const res = buildInitFactsTemplate(force)
  const lines: string[] = []
  if (res.wrote) {
    lines.push(`Zapisano: ${res.path}`)
    lines.push("")
    lines.push(res.body)
    lines.push("---")
    lines.push("Wykryte podpowiedzi wstawione do sekcji: Architektura, Komendy, Środowisko.")
    lines.push("Sekcje Konwencje i Ryzyka pozostawiono puste — uzupełnij ręcznie.")
    lines.push("Auto-fakty (.auto.md) są regenerowane oddzielnie na session.idle lub /codemem auto-refresh.")
  } else {
    lines.push(`NIE zapisano: ${res.skipped}`)
    lines.push("Aby zobaczyć proponowany szablon bez zapisu, edytuj ręcznie lub użyj /codemem init --force.")
  }
  return lines.join("\n")
}

function memoryTestHistory(): string {
  if (!testHistory.length) return "Brak zarejestrowanych uruchomień testów/buildów."
  const rows = testHistory.slice(-15).reverse().map((t) => {
    const status = t.exitCode === 0 ? "OK" : `FAIL(${t.exitCode})`
    const failed = t.failed.length ? `\n    failed: ${t.failed.slice(0, 5).join("; ")}` : ""
    return `[${t.timestamp}] ${status}  ${t.command}\n    ${t.summary}${failed}`
  })
  return ["=== Test history (najnowsze na górze) ===", ...rows].join("\n")
}

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

// Workaround: opencode-1.18.15 nie przekazuje options z krotki [path, opts]
// w opencode.json do local-plugin (patrz options-dump.json -> optionsKeys: []).
// Czytaj opencode.json ręcznie i wyciągnij blok "ai" dla tego pluginu.
function loadAiConfigFromFile(root: string): Partial<AIConfig> | null {
  const candidates = [
    join(root, "opencode.json"),
    join(root, "opencode.jsonc"),
    join(root, ".opencode", "opencode.json"),
  ]
  for (const path of candidates) {
    if (!existsSync(path)) continue
    let raw: string
    try { raw = readFileSync(path, "utf8") } catch { continue }

    // (1) Szybka ścieżka: surowy JSON (TAB-y są poprawne). Plik bez komentarzy.
    let parsed: any = null
    try { parsed = JSON.parse(raw) } catch {}

    // (2) Fallback: usuń komentarze, ale string-aware (nie tnij // wewnątrz "...").
    if (!parsed) {
      const stripped = stripJsonComments(raw)
      try { parsed = JSON.parse(stripped) } catch { continue }
    }

    const plugins = parsed?.plugin
    if (!Array.isArray(plugins)) continue
    for (const entry of plugins) {
      let opts: any = null
      let pluginPath: string = ""
      if (typeof entry === "string") { pluginPath = entry; opts = null }
      else if (Array.isArray(entry) && entry.length >= 2) { pluginPath = String(entry[0]); opts = entry[1] }
      else if (entry && typeof entry === "object") { pluginPath = String(entry.path ?? entry.id ?? ""); opts = entry.options ?? entry }
      if (!pluginPath.includes("project-context")) continue
      if (opts && typeof opts === "object" && opts.ai && typeof opts.ai === "object") {
        return opts.ai as Partial<AIConfig>
      }
    }
  }
  return null
}

// String-aware comment stripper. Omijamy // i /* */ wewnątrz "...".
// Naiwny regex /\/\/[^\n]*/g tnie URL-e (http://) i psuje parsowanie.
function stripJsonComments(src: string): string {
  let out = ""
  let i = 0
  let inString = false
  let escape = false
  const n = src.length
  while (i < n) {
    const ch = src[i]
    const next = src[i + 1]
    if (inString) {
      out += ch
      if (escape) { escape = false }
      else if (ch === "\\") { escape = true }
      else if (ch === '"') { inString = false }
      i++
      continue
    }
    // Poza stringiem: wykryj komentarz
    if (ch === '"') { inString = true; out += ch; i++; continue }
    if (ch === '/' && next === '/') {
      while (i < n && src[i] !== '\n') i++
      continue
    }
    if (ch === '/' && next === '*') {
      i += 2
      while (i < n && !(src[i] === '*' && src[i + 1] === '/')) i++
      i += 2
      continue
    }
    out += ch
    i++
  }
  return out
}

export const ProjectContextPlugin: Plugin = async ({ project, client, $, directory, worktree }, options) => {
  const resolvedWorktree = resolveWorktree(worktree, directory)
  initMemoryLayout(resolvedWorktree)
  pluginClient = client

  const userConfig = (options ?? {}) as Partial<Config>
  cfg = { ...DEFAULT_CONFIG, ...userConfig }

  // Workaround: jeśli options.ai nie przyszedł (puste options), doładuj z opencode.json
  if (!userConfig.ai || !userConfig.ai.enabled) {
    const root = resolvedWorktree
    const fileAi = loadAiConfigFromFile(root)
    if (fileAi) {
      cfg.ai = { ...DEFAULT_CONFIG.ai, ...fileAi }
    }
  }

  // Zastosuj timeout override z /codemem ai auto-timeout (jeśli zapisany w cache).
  // Override = maxObservedMs * 1.3; przeżywa restarty bo zapisany w pliku.
  const overrideMs = aiLoadTimeoutOverride()
  if (overrideMs > 0) cfg.ai.timeoutMs = overrideMs

  try { writeFileSync(join(memoryDir, "cache", "options-dump.json"), JSON.stringify({ optionsRaw: options, optionsKeys: Object.keys(options ?? {}), cfgAi: cfg.ai, aiEffective: aiEffectiveConfig() }, null, 2)) } catch {}

  // Reset AI counters at plugin init (before session.created) — TUI czyta
  // metrics.json przy starcie; bez tego pokazuje stare dane z poprzedniej sesji
  // (np. "active 30021ms 2/4 ok"). Ustawiamy unknown + 0 calls, a session.created
  // health-check nadpisze na active/offline.
  const initAiCfg = aiEffectiveConfig()
  if (initAiCfg) {
    metrics.aiEnabled = true
    metrics.aiProvider = initAiCfg.provider
    metrics.aiModel = initAiCfg.model
    metrics.aiCalls = 0
    metrics.aiSuccesses = 0
    metrics.aiFailures = 0
    metrics.aiLastCallMs = 0
    metrics.aiLastError = ""
    metrics.aiHealthState = "unknown"
    metrics.aiBusy = false
    metrics.aiBusyLabel = ""
    metrics.aiBusySince = 0
    metrics.aiConfigTimeoutMs = initAiCfg.timeoutMs
    metrics.aiMaxObservedMs = aiStatus.maxObservedMs
    metrics.aiLastDurationMs = 0
    metrics.aiTimeoutWarn = false
    metrics.aiTimeoutExtended = false
    failOpen(() => writeJson(metricsPath(), metrics), "flushMetrics at plugin init")
  }

  return {
    // ------------------------------------------------------ session lifecycle
    event: async ({ event }: { event: any }) => {
      await failOpenAsync(async () => {
        const type = event?.properties?.type ?? event?.type
        if (type === "session.created") {
          const sessionId = event?.properties?.info?.sessionID ?? event?.properties?.sessionId ?? ""
          lastSessionId = sessionId
          currentSessionId = sessionId
          metrics.sessionId = sessionId
          compactSuggestionShown = false
          lastContextTokens = 0
          // Addition 2: start fresh per-session trace
          sessionTrace = {
            sessionId,
            buildTestCommands: {},
            editedFiles: {},
            blockers: [],
            startedAt: new Date().toISOString(),
          }
          // Auto-extract facts at session start so context is fresh even on first run
          if (cfg.autoExtractFacts) failOpen(() => refreshAutoFacts(), "refreshAutoFacts on session.created")
          const facts = readProjectFacts()
          const sess = readActiveSession()
          const git = await gitInfo($)
          const block = buildInjectedContext(facts, sess, git)
          lastInjectedContext = block
          // Wstrzykiwanie przez experimental.chat.system.transform (niewidoczne
          // w oknie czatu — kontekst trafia do system prompta modela).
          pendingSystemContext = block
          // Reset synchroniczny aiStatus PRZED health check — TUI nie pokaże
          // starych liczników/czasów z poprzedniej sesji (np. "30003ms 1/2 ok"),
          // tylko "checking…" na czysto.
          const aiCfg = aiEffectiveConfig()
          if (aiCfg) {
            aiStatus.enabled = true
            aiStatus.provider = aiCfg.provider
            aiStatus.model = aiCfg.model
            aiStatus.calls = 0
            aiStatus.successes = 0
            aiStatus.failures = 0
            aiStatus.lastCallMs = 0
            aiStatus.lastError = ""
            aiStatus.healthState = "unknown"
            aiStatus.busy = false
            aiStatus.busyLabel = ""
            aiStatus.busySince = 0
            flushMetrics()
          }
          // Health check AI: wyslij krotki ping, by zweryfikowac dostepnosc modelu
          // i nadpisaj aiStatus. Fire-and-forget — nie blokuje pipeline.
          failOpenAsync(async () => {
            await aiHealthCheck()
            flushMetrics()
          }, "aiHealthCheck on session.created")
          failOpen(() => flushMetricsHeavy(), "flushMetricsHeavy on session.created")
        } else if (type === "session.idle" || type === "session.compacted") {
          const sessionId = event?.properties?.info?.sessionID ?? lastSessionId ?? ""
          // Use execSync-based helper: the Bun shell `$` may be unavailable
          // in some plugin runtimes ("$ is not a function" crash on session.idle)
          const edits = gitStatusPorcelain()
          const handoff = buildHandoff(sessionId, edits)
          writeActiveSession(handoff)
          // AI enhancement: krótki podsumowany status sesji (async, fallback = puste)
          // #1: skip gdy brak aktywności (brak edycji + brak testStatus) — nie ma co podsumowywać.
          // #3: throttle — min. odstęp między auto-wywołaniami AI (domyślnie 10 min).
          const hasActivity = edits.length > 0 || !!handoff.testStatus
          const aiThrottled = hasActivity && aiAutoThrottled()
          if (hasActivity && !aiThrottled) {
            failOpenAsync(async () => {
              const summary = await aiSummarizeSession(edits, handoff.testStatus ? {
                command: handoff.testStatus.lastCommand,
                exitCode: handoff.testStatus.exitCode,
                summary: handoff.testStatus.summary,
              } : undefined)
              if (summary) {
                const s = readActiveSession()
                if (s) {
                  s.currentStatus = summary.slice(0, 400)
                  writeActiveSession(s)
                }
              }
            }, "aiSummarizeSession")
            aiAutoMarkRun()
          }
          // Addition 2: fold per-session trace into persistent aggregated trace
          failOpen(() => mergeTraceIntoGlobal(), "mergeTraceIntoGlobal")
          // Auto-extract deterministic facts (build/test/architecture/environment)
          if (cfg.autoExtractFacts && cfg.autoExtractOnEvents.includes(type)) {
            failOpen(() => refreshAutoFacts(), `refreshAutoFacts on ${type}`)
            // AI enhancement: konwencje/ryzyka z README/CLAUDE.md — działa w tle,
            // wyzwalane tylko gdy pliki źródłowe uległy zmianie (hash w cache), bez throttlingu czasowego.
            failOpenAsync(async () => {
              const humanFacts = await aiExtractHumanFacts(worktreePath || projectRoot)
              if (humanFacts) {
                const p = join(memoryDir, "project-facts.ai.md")
                writeFileSync(p, `# AI-ekstrahowane fakty (regenerowane przy session.idle)\n# Nie edytuj ręcznie; usuń plik by wyłączyć.\n\n${humanFacts}\n`, "utf8")
              }
            }, "aiExtractHumanFacts")
          }
          flushMetrics()
          failOpen(() => flushMetricsHeavy(edits.length), "flushMetricsHeavy")
        } else if (type === "session.deleted") {
          // optional: remove non-persistent session data
          failOpen(() => {
            const sid = event?.properties?.info?.sessionID
            if (sid) rmSync(join(memoryDir, "session-history", `${sid}.json`), { force: true })
          }, "session.deleted cleanup")
        } else if (type === "lsp.client.diagnostics") {
          // TODO: SDK v1.18.11 nie eksponuje diagnostyk LSP — EventLspClientDiagnostics
          // ma tylko { serverID, path }, bez listy błędów. Endpoint /lsp zwraca tylko
          // status serwera (connected/error). Gdy SDK udostępni diagnostyki, wyciągnij
          // je tutaj i zapisz w sess.lspErrors. Pozostawiono jako no-op, aby utrzymać
          // strukturę i sygnalizować brakujące API.
          const sess = readActiveSession()
          if (sess) {
            sess.lspErrors = []
            writeActiveSession(sess)
          }
        } else if (type === "file.edited") {
          const fp: string = event?.properties?.path ?? event?.properties?.info?.path ?? ""
          if (fp) {
            // invalidate read cache for that file (Addition 1: persistent cache)
            for (const [k, s] of seen) {
              if (s.filePath === fp) seen.delete(k)
            }
            failOpen(() => saveDedupCache(), "saveDedupCache on file.edited")
            // Addition 2: track edit count in session trace
            const rel = failOpenReturn(() => relative(worktreePath, fp) || fp, fp, "relative path")
            sessionTrace.editedFiles[rel] = (sessionTrace.editedFiles[rel] ?? 0) + 1
            failOpen(() => saveSessionTrace(), "saveSessionTrace on file.edited")
            const sess = readActiveSession()
            if (sess) {
              if (!sess.modifiedFiles.includes(fp)) sess.modifiedFiles.push(fp)
              writeActiveSession(sess)
            }
          }
        } else if (type === "command.executed") {
          const cmd: string = event?.properties?.command ?? ""
          if (cmd.startsWith("/codemem ") || cmd.startsWith("/context ")) {
            // handled below via tui.command.execute; nothing here
          }
          // CLI fallback: /codemem tui działa też w trybie non-interactive
          if (cmd.startsWith("/codemem tui")) {
            const out: { result?: string } = {}
            await failOpenAsync(async () => { out.result = memoryTuiDump() }, "command.executed /codemem tui")
            if (out.result) console.log(out.result)
          }
        } else if (type === "message.updated") {
          await updateContextTokensFromMessage(event, client)
        }
      }, `event:${event?.type ?? "?"}`)
    },

    // -------------------------------------- experimental compaction hooks
    "experimental.session.compacting": async (input: any, output: any) => {
      await failOpenAsync(async () => {
        // Wzbogać kontekst po kompaktacji o project-facts + handoff,
        // aby agent zachował istotny kontekst projektu.
        if (cfg.autoExtractFacts) failOpen(() => refreshAutoFacts(), "refreshAutoFacts on compacting")
        const facts = readProjectFacts()
        const sess = readActiveSession()
        const extra: string[] = []
        if (facts) extra.push("PROJECT FACTS:\n" + facts)
        if (sess) {
          const handoffBits: string[] = []
          if (sess.goal) handoffBits.push(`Cel: ${sess.goal}`)
          if (sess.currentStatus) handoffBits.push(`Status: ${sess.currentStatus}`)
          if (sess.modifiedFiles?.length) handoffBits.push(`Edytowane pliki: ${sess.modifiedFiles.slice(0, 10).join(", ")}`)
          if (sess.blockers?.length) handoffBits.push(`Blokerzy: ${sess.blockers.join("; ")}`)
          if (sess.testStatus) handoffBits.push(`Ostatni test (${sess.testStatus.exitCode}): ${sess.testStatus.summary}`)
          if (handoffBits.length) extra.push("SESSION HANDOFF:\n" + handoffBits.join("\n"))
        }
        if (extra.length) {
          if (!Array.isArray(output.context)) output.context = []
          ;(output.context as string[]).push(...extra)
        }
      }, "experimental.session.compacting")
    },

    "experimental.compaction.autocontinue": async (input: any, output: any) => {
      await failOpenAsync(async () => {
        // Reset flagi sugestii po kompaktacji; zostaw autocontinue włączone.
        compactSuggestionShown = false
        lastContextTokens = 0
        if (cfg.compactMode === "off") output.enabled = true
      }, "experimental.compaction.autocontinue")
    },

    // Wstrzykiwanie kontekstu projektu do system prompta (niewidoczne w oknie
    // czatu). Dokłada blok PROJECT MEMORY (fakty + handoff + git) do output.system.
    "experimental.chat.system.transform": async (input: any, output: any) => {
      await failOpenAsync(async () => {
        if (!pendingSystemContext) return
        if (!Array.isArray(output.system)) output.system = []
        ;(output.system as string[]).push(pendingSystemContext)
      }, "experimental.chat.system.transform")
    },

    // ----------------------------------------------------- tool.execute hooks
    "tool.execute.before": async (input: any, output: any) => {
      await failOpenAsync(async () => {
        const t = input?.tool
        // security: block sensitive reads
        if (t === "read") {
          const fp: string = output?.args?.filePath ?? ""
          if (isSensitivePath(fp)) {
            throw new Error(`Blocked read of sensitive file: ${fp}. Use /codemem to allow explicitly.`)
          }
        }
        // security: block bash reading secrets
        if (t === "bash") {
          const c: string = output?.args?.command ?? ""
          if (/\b(cat|type|Get-Content)\s+.*(\.env|id_rsa|\.pem|credentials|secrets)\b/i.test(c)) {
            throw new Error("Blocked command that reads secrets.")
          }
        }
      }, "tool.execute.before")
    },

    "tool.execute.after": async (input: any, output: any) => {
      await failOpenAsync(async () => {
        const t = input?.tool
        const args = output?.args ?? input?.args ?? {}
        const result = output?.result ?? output?.output ?? output?.content ?? ""
        if (typeof result !== "string" || !result) return
        const exitCode = output?.exitCode ?? output?.code ?? 0
        const filtered = filterToolResult(t, args, result, exitCode)
        if (filtered !== result) {
          output.result = filtered
          output.content = filtered
          output.output = filtered
        }
      }, "tool.execute.after")
      flushMetrics()
    },

    // ----------------------------------------------------------- custom tool
    tool: {
      read_artifact: tool({
        description: "Read a previously stored full tool result artifact by its short id, with pagination and optional search.",
        args: {
          artifactId: tool.schema.string(),
          offset: tool.schema.number().optional(),
          limit: tool.schema.number().optional(),
          search: tool.schema.string().optional(),
        },
        async execute(args: any, _ctx: any) {
          return readArtifact(args.artifactId, args.offset ?? 0, args.limit ?? cfg.maxArtifactPreviewLines, args.search)
        },
      }),
    },

    // ------------------------------------------------------ command interception
    // UWAGA (root cause 2026-08-04): komendy z .opencode/command/*.md ZAWSZE
    // trafiają do modelu jako prompt (command.execute.before -> prompt() -> LLM);
    // opencode nie wspiera jeszcze pominięcia LLM (feature request #28292 noReply).
    // Dlatego, gdy plugin jest aktywny, wynik deterministyczny liczymy tu i
    // zapisujemy do .opencode/memory/command_result.txt; szablon komendy każe
    // modelowi zwrócić ten plik 1:1 (gwarantowany determinizm), a gdy pluginu
    // brak — model działa wg jawnego opisu w szablonie.
    "command.execute.before": async (input: any, output: any) => {
      await failOpenAsync(async () => {
        // opencode splits the typed slash command into `command` (e.g. "/memory"
        // or "memory") and `arguments` (e.g. "ai models"). Reconstruct the full
        // token stream so dispatchCommand's startsWith checks can match subcommands.
        const cmdPart = String(input?.command ?? input?.input?.command ?? "")
        const argPart = input?.arguments != null ? " " + String(input.arguments) : ""
        const raw = `${cmdPart}${argPart}`.trim()
        const norm = raw.trim()
        if (!/^\/?((codemem|context|regression)\b)/.test(norm)) return
        lastUserCommandTs = Date.now()
        const slash = norm.startsWith("/") ? norm : "/" + norm
        const result = await dispatchCommand(slash)
        if (result === undefined) return
        try {
          writeFileSync(commandResultPath(), `# ${slash}\n${result}`, "utf8")
        } catch { /* ignore */ }
      }, "command.execute.before")
    },

    // ----------------------------------------------------------- TUI commands
    "tui.command.execute": async (input: any, output: any) => {
      await failOpenAsync(async () => {
        const cmdPart: string = input?.command ?? ""
        const argPart = input?.arguments != null ? " " + String(input.arguments) : ""
        const cmd: string = `${cmdPart}${argPart}`.trim()
        lastUserCommandTs = Date.now()
        const r = await dispatchCommand(cmd)
        if (r !== undefined) output.result = r
      }, "tui.command.execute")
    },
  }
}

function commandResultPath(): string {
  return join(memoryDir, "command_result.txt")
}

// Współdzielony dispatch komend memory/context/regression → deterministyczny wynik.
// async: compact-now i ai triage wymagają await (wołają client API / aiComplete).
async function dispatchCommand(cmd: string): Promise<string | undefined> {
  if (cmd.startsWith("/codemem status")) return memoryStatus()
  if (cmd.startsWith("/codemem show")) return memoryShow()
  if (cmd.startsWith("/codemem save")) {
    const handoff = buildHandoff(lastSessionId, [])
    writeActiveSession(handoff)
    return "Handoff saved."
  }
  if (cmd.startsWith("/codemem clear-session")) return memoryClearSession()
  if (cmd.startsWith("/codemem clear-project")) return memoryClearProject()
  if (cmd.startsWith("/codemem compact-status")) return memoryCompactStatus()
  if (cmd.startsWith("/codemem compact-reset")) return memoryCompactReset()
  if (cmd.startsWith("/codemem compact-now")) return memoryCompactNow(pluginClient)
  if (cmd.startsWith("/codemem compact")) {
    const handoff = buildHandoff(lastSessionId, [])
    writeActiveSession(handoff)
    return "Compact handoff created."
  }
  if (cmd.startsWith("/codemem propose")) return memoryPropose()
  if (cmd.startsWith("/codemem commit")) return commitProposedFacts()
  if (cmd.startsWith("/codemem lesson")) return memoryLesson(cmd.replace(/^\/codemem lesson\s*/, ""))
  if (cmd.startsWith("/codemem auto-refresh")) return memoryAutoRefresh()
  if (cmd.startsWith("/codemem auto")) return memoryAutoShow()
  if (cmd.startsWith("/codemem init")) return memoryInit(cmd.replace(/^\/codemem init\s*/, ""))
  if (cmd.startsWith("/codemem test-history")) return memoryTestHistory()
  if (cmd.startsWith("/codemem ai status")) return aiStatusText()
  if (cmd.startsWith("/codemem ai auto-timeout")) return aiAutoTimeoutCommand()
  if (cmd.startsWith("/codemem ai triage")) return aiTriageFailedTests()
  if (cmd.startsWith("/codemem ai")) return aiStatusText()
  if (cmd.startsWith("/codemem tui")) return memoryTuiDump()
  if (cmd.startsWith("/codemem exit")) return codememExit(pluginClient)
  if (cmd.startsWith("/codemem dashboard")) return "Dashboard TUI: użyj w trybie interaktywnym (route: memory-dashboard)"
  if (cmd.startsWith("/context budget")) return contextBudget()
  if (cmd.startsWith("/context artifacts")) return contextArtifacts()
  if (cmd.startsWith("/regression last-good")) return regressionLastGood()
  if (cmd.startsWith("/regression suspect")) return regressionSuspect()
  if (cmd.startsWith("/regression revert")) return regressionRevert(cmd.replace(/^\/regression revert\s*/, ""))
  if (cmd.startsWith("/regression feature")) return regressionFeature(cmd.replace(/^\/regression feature\s*/, ""))
  return undefined
}

// Eksport testowy — funkcje czysto/jednostkowe do pokrycia regresyjnego.
export const __testState = () => ({
  interpolateEnv,
  aiUrl,
  safeErrorString,
  parseModelKey,
})

export default ProjectContextPlugin