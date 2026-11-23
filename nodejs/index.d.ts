import type http from "node:http";

export declare const DEFAULT_MODEL: "gemini-auto";
export declare const ACCOUNTS_COOKIE_PREFIX: "GwaAccounts.";

export declare const AccountTier: Readonly<{
  UNKNOWN: -1;
  FREE: 0;
  PRO: 1;
  ULTRA: 2;
}>;
export type AccountTierValue = -1 | 0 | 1 | 2;

export interface WebModel {
  id: string;
  label: string;
  name: string;
  available: boolean;
  note: string;
}

export interface ChatThread {
  conversation: string;
  response: string;
  candidate: string;
}

export interface AskResult {
  answer: string;
  thinking: string;
  servedLabel: string;
  servedNumber: number;
  thread: ChatThread;
  elapsedMs: number;
}

export interface UsageWindow {
  valid: boolean;
  usedPercent: number;
  resetsAtMs: number;
}

export interface UsageMeter {
  valid: boolean;
  shortWindow: UsageWindow;
  weekly: UsageWindow;
}

export interface ClientOptions {
  account?: number;
  timeoutMs?: number;
  generateTimeoutMs?: number;
  allowUltra?: boolean;
  onCookieUpdate?: (account: number, updatedHeader: string) => void;
}

export interface AskOptions {
  model?: string;
  effort?: "low" | "medium" | "high" | string;
  images?: Array<string | { bytes: Buffer | Uint8Array; filename?: string; mimeType?: string }>;
  continueChat?: boolean;
  thread?: ChatThread;
  onPartial?: (snapshot: string) => void;
  retries?: number;
}

export declare class CookieJar {
  header: string;
  sapisid: string;
  email: string;
  count: number;
  account: number;
  readonly liveHeader: string;
  scopedHeader(forAccounts?: boolean): string;
  hashKey(account?: number): string;
  accounts(): number[];
  absorbSetCookies(setCookieHeaders: string[], opts?: { fromAccounts?: boolean }): string | null;
}

export declare class GeminiWebError extends Error {}
export declare class AuthRejectedError extends GeminiWebError {}

export declare class GeminiWebClient {
  jar: CookieJar;
  thread: ChatThread;
  readonly liveCookies: string;
  constructor(cookies: string | CookieJar, options?: ClientOptions);
  resetChat(): void;
  resetSession(): void;
  listModels(options?: { force?: boolean }): Promise<{ models: WebModel[]; tier: AccountTierValue; email: string }>;
  uploadImage(
    source: string | Buffer | Uint8Array,
    options?: { filename?: string; mimeType?: string; pushId?: string }
  ): Promise<{ fileId: string; fileName: string }>;
  ask(prompt: string, options?: AskOptions): Promise<AskResult>;
  stream(prompt: string, options?: AskOptions): AsyncGenerator<string, void, unknown>;
  getUsage(): Promise<UsageMeter>;
  rotateCookies(options?: { force?: boolean }): Promise<boolean>;
}

export interface BridgeServerOptions {
  defaultModel?: string;
  defaultEffort?: string;
  apiKey?: string | null;
}

export declare function parseCookieHeader(text: string, account?: number): CookieJar;
export declare function mergeCookieHeaders(existing: string, incoming: string): string;
export declare function rotatingCookieChanged(previous: string, current: string, account?: number): boolean;
export declare function stripCitations(text: string): string;
export declare function usageSummary(meter: UsageMeter, nowMs?: number): string;
export declare function createBridgeServer(client: GeminiWebClient, options?: BridgeServerOptions): http.Server;
