import type {
  Character,
  GroupChat,
  LiveEvent,
  ProviderStatus,
  RuntimeStatus,
  Session,
  VoiceInfo,
} from "./types";

const API = "";

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${API}${path}`, {
    ...init,
    headers: {
      "Content-Type": "application/json",
      ...(init?.headers || {}),
    },
  });
  if (!response.ok) {
    let detail = response.statusText;
    try {
      const body = await response.json();
      detail = body.detail || body.error || JSON.stringify(body);
    } catch {
      detail = await response.text();
    }
    throw new Error(typeof detail === "string" ? detail : JSON.stringify(detail));
  }
  return response.json() as Promise<T>;
}

export const api = {
  session: (id: string) => request<Session>(`/v1/sessions/${id}`),
  health: () => request<{ ok: boolean }>("/v1/health"),
  status: () => request<RuntimeStatus>("/v1/status"),
  characters: () => request<Character[]>("/v1/characters"),
  createCharacter: (payload: Partial<Character>) =>
    request<Character>("/v1/characters", { method: "POST", body: JSON.stringify(payload) }),
  updateCharacter: (id: string, payload: Partial<Character>) =>
    request<Character>(`/v1/characters/${id}`, { method: "PATCH", body: JSON.stringify(payload) }),
  deleteCharacter: (id: string) => request(`/v1/characters/${id}`, { method: "DELETE" }),
  sessions: (characterId: string) => request<Session[]>(`/v1/sessions?character_id=${characterId}`),
  createSession: (characterId: string, game_context: Record<string, unknown> = {}) =>
    request<Session>("/v1/sessions", {
      method: "POST",
      body: JSON.stringify({ character_id: characterId, game_context }),
    }),
  getSession: (id: string) => request<Session>(`/v1/sessions/${id}`),
  setContext: (id: string, game_context: Record<string, unknown>, character_state?: Record<string, unknown>) =>
    request<Session>(`/v1/sessions/${id}/context`, {
      method: "POST",
      body: JSON.stringify({ game_context, character_state }),
    }),
  clearSession: (id: string) => request<Session>(`/v1/sessions/${id}/clear`, { method: "POST" }),
  voices: () => request<VoiceInfo[]>("/v1/voices"),
  previewVoice: (voice_id: string, text: string, speed: number) =>
    request<{ pcm16_b64: string; sample_rate: number }>("/v1/voices/preview", {
      method: "POST",
      body: JSON.stringify({ voice_id, text, speed }),
    }),
  providers: () => request<ProviderStatus[]>("/v1/providers"),
  getConfig: () => request<Record<string, unknown>>("/v1/config"),
  patchConfig: (payload: Record<string, unknown>) =>
    request("/v1/config", { method: "PATCH", body: JSON.stringify(payload) }),
  models: () =>
    request<{
      recommended: Array<{
        id: string;
        name: string;
        description: string;
        installed: boolean;
        path: string;
        bytes_on_disk: number;
        size_bytes: number;
        min_vram_gb: number;
      }>;
      job: { id?: string | null; status: string; error?: string | null; bytes_on_disk?: number; size_bytes?: number };
    }>("/v1/models"),
  installModel: (id: string) => request<{ status: string; path?: string; error?: string }>("/v1/models/install", {
    method: "POST",
    body: JSON.stringify({ id }),
  }),
  groups: () => request<GroupChat[]>("/v1/groups"),
  createGroup: (character_ids: string[], title?: string) =>
    request<GroupChat>("/v1/groups", {
      method: "POST",
      body: JSON.stringify({ character_ids, title: title || undefined, player_name: "You" }),
    }),
  getGroup: (id: string) => request<GroupChat>(`/v1/groups/${id}`),
  deleteGroup: (id: string) => request<{ ok: boolean }>(`/v1/groups/${id}`, { method: "DELETE" }),
};

export function liveUrl(sessionId: string): string {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${location.host}/v1/sessions/${sessionId}/live`;
}

export function groupLiveUrl(groupId: string): string {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${location.host}/v1/groups/${groupId}/live`;
}

export function connectLive(sessionId: string, onEvent: (event: LiveEvent) => void): WebSocket {
  const socket = new WebSocket(liveUrl(sessionId));
  socket.onmessage = (message) => {
    onEvent(JSON.parse(message.data) as LiveEvent);
  };
  return socket;
}

export function connectGroupLive(groupId: string, onEvent: (event: LiveEvent) => void): WebSocket {
  const socket = new WebSocket(groupLiveUrl(groupId));
  socket.onmessage = (message) => {
    onEvent(JSON.parse(message.data) as LiveEvent);
  };
  return socket;
}
