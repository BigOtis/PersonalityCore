export type ConversationState =
  | "idle"
  | "listening"
  | "transcribing"
  | "thinking"
  | "speaking"
  | "error";

export type ActionCue = {
  name: string;
  target?: string | null;
  parameters?: Record<string, unknown>;
};

export type CharacterReply = {
  dialogue: string;
  emotion?: string | null;
  intent?: string | null;
  actions: ActionCue[];
  animation?: string | null;
  world_interactions: ActionCue[];
  tool_requests: ActionCue[];
  state_changes: Record<string, unknown>;
};

export type VoiceSettings = {
  voice_id: string;
  speed: number;
  language: string;
};

export type Character = {
  id: string;
  name: string;
  personality: string;
  instructions: string;
  provider_id?: string | null;
  model?: string | null;
  voice: VoiceSettings;
  color: string;
};

export type ChatMessage = {
  id: string;
  role: "system" | "user" | "assistant";
  content: string;
  reply?: CharacterReply | null;
  created_at: string;
  speaker?: string;
  name?: string;
};

export type Session = {
  id: string;
  character_id: string;
  title: string;
  messages: ChatMessage[];
  game_context: Record<string, unknown>;
  character_state: Record<string, unknown>;
};

export type GroupMember = {
  key: string;
  character_id?: string | null;
  character: {
    name: string;
    personality?: string;
    instructions?: string;
    color?: string;
    voice?: VoiceSettings;
  };
  public_description?: string;
};

export type GroupChat = {
  id: string;
  kind: "group" | "scene";
  definition: {
    name: string;
    members: GroupMember[];
    goal?: string;
    guidance?: string;
  };
  history: Array<{
    speaker: string;
    name: string;
    text: string;
    target?: string;
    reply?: CharacterReply | null;
    turn_id?: string;
  }>;
  player_name: string;
  joined: boolean;
  speaker: string;
  state: string;
  turn_id: string;
};

export type TimingReport = {
  events: { name: string; at_ms: number; detail?: string | null }[];
  totals_ms: Record<string, number>;
};

export type VoiceInfo = {
  id: string;
  name: string;
  language: string;
  gender?: string | null;
  traits?: string | null;
};

export type ProviderStatus = {
  provider: {
    id: string;
    name: string;
    kind: string;
    base_url: string;
    model: string;
    enabled: boolean;
  };
  reachable: boolean;
  models: string[];
  error?: string | null;
  latency_ms?: number | null;
};

export type RuntimeStatus = {
  default_provider_id?: string;
  inference_ready?: boolean;
  ready: boolean;
  version: string;
  state: ConversationState;
  speech: Record<string, unknown>;
  gpu: Record<string, unknown>;
  errors: string[];
  provider_status?: ProviderStatus[];
};

export type LiveEvent = {
  type: string;
  state?: ConversationState;
  text?: string;
  delta?: string;
  reply?: CharacterReply;
  timing?: TimingReport;
  error?: string;
  pcm16_b64?: string;
  sample_rate?: number;
  chunk_index?: number;
  voice_id?: string;
  transcript?: string;
  message_id?: string;
  speaker?: string;
  name?: string;
  turn_id?: string;
  remaining?: number;
  awaiting_playback?: boolean;
  extra?: Record<string, unknown>;
  scene?: GroupChat;
};
