import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { api, connectGroupLive, connectLive } from "./api";
import { MicCapture, PlaybackQueue, base64ToInt16, listDevices, pcm16ToBase64 } from "./audio";
import type {
  Character,
  ChatMessage,
  ConversationState,
  GroupChat,
  LiveEvent,
  ProviderStatus,
  RuntimeStatus,
  Session,
  TimingReport,
  VoiceInfo,
} from "./types";

const STATE_LABEL: Record<ConversationState, string> = {
  idle: "Ready",
  listening: "Listening",
  transcribing: "Transcribing",
  thinking: "Thinking",
  speaking: "Speaking",
  error: "Error",
};

export function App() {
  const [characters, setCharacters] = useState<Character[]>([]);
  const [characterId, setCharacterId] = useState<string>("");
  const [groupId, setGroupId] = useState("");
  const [groups, setGroups] = useState<GroupChat[]>([]);
  const [group, setGroup] = useState<GroupChat | null>(null);
  const [creatingGroup, setCreatingGroup] = useState(false);
  const [speakTo, setSpeakTo] = useState("all");
  const [streamingName, setStreamingName] = useState("");
  const [sessionList, setSessionList] = useState<Session[]>([]);
  const [session, setSession] = useState<Session | null>(null);
  const [status, setStatus] = useState<RuntimeStatus | null>(null);
  const [providers, setProviders] = useState<ProviderStatus[]>([]);
  const [voices, setVoices] = useState<VoiceInfo[]>([]);
  const [state, setState] = useState<ConversationState>("idle");
  const [draft, setDraft] = useState("");
  const [streaming, setStreaming] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [tab, setTab] = useState<"reply" | "context" | "timing" | "setup">("reply");
  const [timing, setTiming] = useState<TimingReport | null>(null);
  const [lastReply, setLastReply] = useState<ChatMessage["reply"]>(null);
  const [contextText, setContextText] = useState("{\n  \"location\": \"developer studio\"\n}");
  const [inspectorOpen, setInspectorOpen] = useState(false);
  const [connected, setConnected] = useState(false);
  const [playing, setPlaying] = useState(false);
  const [muted, setMuted] = useState(false);
  const [level, setLevel] = useState(0);
  const [firstAudioMs, setFirstAudioMs] = useState<number | null>(null);
  const [editor, setEditor] = useState<Character | null>(null);
  const [creating, setCreating] = useState(false);
  const [devices, setDevices] = useState<{ inputs: MediaDeviceInfo[]; outputs: MediaDeviceInfo[] }>({
    inputs: [],
    outputs: [],
  });
  const [micId, setMicId] = useState("");
  const [outId, setOutId] = useState("");
  const [listenMode, setListenMode] = useState<"push_to_talk" | "auto">("push_to_talk");
  const [armed, setArmed] = useState(false);
  const [boot, setBoot] = useState("Connecting to LocalTalker…");

  const socketRef = useRef<WebSocket | null>(null);
  const playback = useRef(new PlaybackQueue());
  const mic = useRef(new MicCapture());
  const scroller = useRef<HTMLDivElement | null>(null);
  const draftRef = useRef("");
  const armedRef = useRef(false);
  const mutedRef = useRef(false);
  const interruptedRef = useRef(false);
  const turnStart = useRef(0);
  const heardAudio = useRef(false);
  const firstAudioRef = useRef<number | null>(null);
  const retryRef = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const groupIdRef = useRef(groupId);
  groupIdRef.current = groupId;
  const speakToRef = useRef(speakTo);
  speakToRef.current = speakTo;
  playback.current.onPlaying = setPlaying;
  playback.current.onFirstAudio = () => {
    if (!heardAudio.current && turnStart.current) {
      heardAudio.current = true;
      firstAudioRef.current = performance.now() - turnStart.current;
      setFirstAudioMs(firstAudioRef.current);
    }
  };
  function showError(exc: unknown) { setError(exc instanceof Error ? exc.message : String(exc)); }


  const character = characters.find((item) => item.id === characterId) || null;

  const refreshStatus = useCallback(async () => {
    const next = await api.status();
    setStatus(next);
    setProviders(next.provider_status || (await api.providers()));
  }, []);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        await api.health();
        const [chars, voiceList, groupList] = await Promise.all([
          api.characters(),
          api.voices(),
          api.groups().catch(() => [] as GroupChat[]),
          refreshStatus(),
        ]);
        if (cancelled) return;
        setCharacters(chars);
        setVoices(voiceList);
        setGroups(groupList);
        if (chars[0]) setCharacterId(chars[0].id);
        setBoot("");
      } catch (exc) {
        setBoot(exc instanceof Error ? exc.message : "Runtime is not reachable");
      }
      try {
        const listed = await listDevices();
        if (!cancelled) setDevices(listed);
      } catch {
        /* permission later */
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [refreshStatus]);

  useEffect(() => {
    if (!characterId || groupId) return;
    let cancelled = false;
    setSession(null);
    setStreaming("");
    setStreamingName("");
    setTiming(null);
    setFirstAudioMs(null);
    setState("idle");
    (async () => {
      const existing = (await api.sessions(characterId)).filter(item => !item.game_context?.group_chat);
      const next = existing[0] || (await api.createSession(characterId));
      if (!cancelled) {
        setSessionList(existing.length ? existing : [next]);
        setSession(next);
        setContextText(JSON.stringify(next.game_context || {}, null, 2));
        setLastReply(next.messages.at(-1)?.reply || null);
      }
    })().catch(showError);
    return () => { cancelled = true; };
  }, [characterId, groupId]);

  useEffect(() => {
    if (!groupId) {
      setGroup(null);
      return;
    }
    let cancelled = false;
    setSession(null);
    setStreaming("");
    setStreamingName("");
    setSpeakTo("all");
    setState("idle");
    api.getGroup(groupId).then(next => {
      if (!cancelled) {
        setGroup(next);
        setLastReply(next.history.at(-1)?.reply || null);
      }
    }).catch(showError);
    return () => { cancelled = true; };
  }, [groupId]);

  useEffect(() => {
    const isGroup = Boolean(groupId);
    const id = isGroup ? groupId : session?.id;
    if (!id) return;
    const liveId = id;
    let disposed = false;
    function connect() {
      if (disposed) return;
      const socket = isGroup
        ? connectGroupLive(liveId, event => { if (!disposed && socketRef.current === socket) onLive(event); })
        : connectLive(liveId, event => { if (!disposed && socketRef.current === socket) onLive(event); });
      socketRef.current = socket;
      socket.onopen = () => {
        if (disposed) return;
        setConnected(true);
        setError(null);
        if (isGroup) socket.send(JSON.stringify({ type: "join", player_name: "You" }));
      };
      socket.onclose = () => {
        if (disposed) return;
        setConnected(false); setState("idle"); setStreaming(""); playback.current.stop();
        mic.current.stop(); armedRef.current = false; setArmed(false);
        setError("Connection lost. Reconnecting...");
        retryRef.current = setTimeout(async () => {
          try {
            if (isGroup) {
              const saved = await api.getGroup(liveId);
              if (!disposed) setGroup(saved);
            } else {
              const saved = await api.session(liveId);
              if (!disposed) setSession(saved);
            }
          } catch { /* retry socket */ }
          connect();
        }, 1500);
      };
    }
    connect();
    return () => {
      disposed = true; clearTimeout(retryRef.current); setConnected(false);
      socketRef.current?.close(); playback.current.stop(); mic.current.stop();
      armedRef.current = false; setArmed(false);
    };
  }, [groupId, session?.id]);

  useEffect(() => {
    const timer = setInterval(() => void refreshStatus().catch(() => {}), 12000);
    return () => clearInterval(timer);
  }, [refreshStatus]);

  useEffect(() => {
    if (!armed) return;
    const timer = setInterval(() => setLevel(mic.current.level()), 80);
    const limit = setTimeout(finishTalk, 60000);
    return () => { clearInterval(timer); clearTimeout(limit); setLevel(0); };
  }, [armed]);

  useEffect(() => {
    scroller.current?.scrollTo({ top: scroller.current.scrollHeight, behavior: "smooth" });
  }, [session?.messages, streaming]);

  useEffect(() => {
    void playback.current.setOutput(outId).catch(showError);
  }, [outId]);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      const tag = (event.target as HTMLElement | null)?.tagName;
      const typing = tag === "TEXTAREA" || tag === "INPUT" || tag === "SELECT" || !!editor || creating || creatingGroup;
      if (event.code === "Space" && !typing && listenMode === "push_to_talk") {
        event.preventDefault();
        if (event.type === "keydown" && !event.repeat) void beginTalk();
        if (event.type === "keyup") finishTalk();
      }
      if (event.key === "Escape") { if (editor || creating || creatingGroup) { setEditor(null); setCreating(false); setCreatingGroup(false); } else interrupt(); }
    };
    window.addEventListener("keydown", onKey);
    window.addEventListener("keyup", onKey);
    return () => {
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("keyup", onKey);
    };
  });

  useEffect(() => {
    if (!armed || listenMode !== "auto") return;
    let heard = false;
    let quiet = 0;
    const timer = window.setInterval(() => {
      const level = mic.current.level();
      if (level > 0.03) {
        heard = true;
        quiet = 0;
      } else if (heard) {
        quiet += 100;
        if (quiet >= 750) finishTalk();
      }
    }, 100);
    return () => window.clearInterval(timer);
  }, [armed, listenMode]);

  function onLive(event: LiveEvent) {
    if (event.type === "scene" && event.scene) {
      setGroup(event.scene);
      return;
    }
    if (event.type === "state" && event.state && !armedRef.current) setState(event.state);
    if (event.type === "scene_state") {
      if (event.state === "transcribing") setState("transcribing");
      else if (event.state === "idle") { setState("idle"); setStreaming(""); setStreamingName(""); }
      else if (event.state) setState(event.state);
    }
    if (event.type === "ready") return;
    if (interruptedRef.current && ["audio", "token", "dialogue", "reply"].includes(event.type)) return;
    if (event.type === "error" && event.error) { setError(event.error); setStreaming(""); setState("idle"); }
    if (event.type === "warning" && event.error) setError(event.error);
    if (event.type === "interrupted") { playback.current.stop(); setStreaming(""); setStreamingName(""); }

    if (event.type === "turn_start") {
      interruptedRef.current = false;
      setStreaming("");
      setStreamingName(event.name || "");
      setState("thinking");
    }
    if (event.type === "dialogue") setStreaming(event.text || "");
    if (event.type === "player_message" && event.text) {
      interruptedRef.current = false;
      setGroup(prev => prev ? {
        ...prev,
        history: [...prev.history, { speaker: "player", name: event.name || "You", text: event.text!, target: undefined }],
      } : prev);
    }
    if (event.type === "user_message" && event.text && !groupIdRef.current) {
      interruptedRef.current = false;
      setSession((prev) =>
        prev
          ? {
              ...prev,
              messages: [
                ...prev.messages,
                { id: event.message_id || `u-${Date.now()}`, role: "user", content: event.text!, created_at: new Date().toISOString() },
              ],
            }
          : prev,
      );
    }
    if (event.type === "reply" && event.reply) {
      setStreaming("");
      setLastReply(event.reply);
      setTiming(event.timing || null);
      if (groupIdRef.current) {
        setGroup(prev => {
          if (!prev) return prev;
          const already = event.turn_id && prev.history.some(line => line.turn_id === event.turn_id);
          if (already) return prev;
          return {
            ...prev,
            history: [...prev.history, {
              speaker: event.speaker || "",
              name: event.name || streamingName || "Character",
              text: event.reply!.dialogue,
              reply: event.reply,
              turn_id: event.turn_id,
            }],
          };
        });
      } else {
        setSession((prev) =>
          prev
            ? {
                ...prev,
                character_state: { ...prev.character_state, ...event.reply!.state_changes },
                messages: [
                  ...prev.messages,
                  {
                    id: event.message_id || `a-${Date.now()}`,
                    role: "assistant",
                    content: event.reply!.dialogue,
                    reply: event.reply,
                    created_at: new Date().toISOString(),
                  },
                ],
              }
            : prev,
        );
      }
    }
    if (event.type === "turn_end") {
      const turnId = event.turn_id;
      void (async () => {
        if (event.awaiting_playback && !mutedRef.current) {
          await playback.current.waitUntilIdle();
        }
        if (turnId && socketRef.current?.readyState === WebSocket.OPEN && !interruptedRef.current) {
          socketRef.current.send(JSON.stringify({ type: "playback_done", turn_id: turnId }));
        }
      })();
    }
    if (event.type === "audio" && event.pcm16_b64 && event.sample_rate) {
      if (!mutedRef.current) void playback.current.enqueue(base64ToInt16(event.pcm16_b64), event.sample_rate).catch(showError);
    }
    if (event.type === "cancelled") {
      setStreaming("");
      setState("idle");
    }
  }

  function send(payload: Record<string, unknown>) {
    const socket = socketRef.current;
    if (socket && socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify(payload));
      return true;
    }
    setError("Conversation socket is not connected");
    return false;
  }

  function sendText(text = draft) {
    const trimmed = text.trim();
    if (!trimmed || !connected || state !== "idle") return;
    playback.current.stop();
    void playback.current.unlock().catch(showError);
    heardAudio.current = false;
    firstAudioRef.current = null; setFirstAudioMs(null); turnStart.current = performance.now();
    if (send({ type: "text", text: trimmed, target: speakToRef.current, player_name: "You" })) {
      setDraft(""); draftRef.current = ""; setError(null); setState("thinking");
    }
  }

  async function beginTalk() {
    if (armedRef.current || !connected) return;
    if (state !== "idle" || playback.current.playing) interrupt();
    setError(null); armedRef.current = true; setArmed(true); setState("listening");
    try {
      await playback.current.unlock();
      if (!armedRef.current) return;
      await mic.current.start(micId || undefined);
      if (!armedRef.current) { mic.current.stop(); return; }
      setDevices(await listDevices());
    } catch (exc) {
      armedRef.current = false; setArmed(false); setState("idle"); showError(exc);
    }
  }

  function finishTalk() {
    if (!armedRef.current) return;
    armedRef.current = false; setArmed(false);
    const captured = mic.current.stop();
    if (!captured.pcm.byteLength) { setState("idle"); return; }
    heardAudio.current = false;
    firstAudioRef.current = null; setFirstAudioMs(null); turnStart.current = performance.now();
    send({ type: "audio", pcm16_b64: pcm16ToBase64(captured.pcm), sample_rate: captured.sampleRate, channels: 1, target: speakToRef.current });
    if (send({ type: "end_audio", target: speakToRef.current })) setState("transcribing");
  }

  function interrupt() {
    interruptedRef.current = true; playback.current.stop();
    mic.current.stop(); armedRef.current = false; setArmed(false);
    if (socketRef.current?.readyState === WebSocket.OPEN) send({ type: "interrupt" });
    setState("idle"); setStreaming("");
  }

  function selectCharacter(id: string) {
    if (id === characterId && !groupId) return;
    interrupt(); setConnected(false); setGroupId(""); setGroup(null); setSpeakTo("all"); setCharacterId(id);
  }

  function selectGroup(id: string) {
    if (id === groupId) return;
    interrupt(); setConnected(false); setCharacterId(""); setSession(null); setGroupId(id);
  }

  async function newConversation() {
    if (groupId || !characterId) return;
    interrupt();
    try {
      const next = await api.createSession(characterId);
      setSessionList(prev => [next, ...prev]);
      openConversation(next);
    } catch (exc) { showError(exc); }
  }

  function openConversation(next: Session) {
    interrupt(); setSession(next); setStreaming(""); setTiming(null); setFirstAudioMs(null);
    setContextText(JSON.stringify(next.game_context, null, 2));
    setLastReply(next.messages.at(-1)?.reply || null);
  }

  async function applyContext() {
    if (!session) return;
    try {
      const parsed = JSON.parse(contextText || "{}");
      const next = await api.setContext(session.id, parsed);
      setSession(next);

    } catch (exc) {
      setError(exc instanceof Error ? exc.message : "Context must be valid JSON");
    }
  }

  async function saveCharacter(next: Character) {
    const saved = await api.updateCharacter(next.id, next);
    setCharacters((prev) => prev.map((item) => (item.id === saved.id ? saved : item)));
    setEditor(null);
  }

  async function createCharacter(next: Partial<Character>) {
    const saved = await api.createCharacter({
      name: next.name || "New character",
      personality: next.personality || "",
      instructions: next.instructions || "",
      provider_id: next.provider_id,
      model: next.model,
      voice: next.voice || { voice_id: "af_bella", speed: 1, language: "en-us" },
    });
    setCharacters((prev) => [...prev, saved]);
    selectCharacter(saved.id);
    setCreating(false);
  }

  async function previewVoice(voiceId: string, speed: number) {
    playback.current.stop();
    await playback.current.unlock();
    const result = await api.previewVoice(voiceId, "The fire is warm. Sit, and tell me what you need.", speed);
    await playback.current.enqueue(base64ToInt16(result.pcm16_b64), result.sample_rate);
  }

  const busy = state === "thinking" || state === "speaking" || state === "transcribing" || playing;
  const displayState = armed ? "listening" : playing ? "speaking" : state;

  const visibleMessages = useMemo(() => {
    if (group) {
      return group.history.map((line, index) => ({
        id: line.turn_id || `g-${index}-${line.speaker}`,
        role: (line.speaker === "player" ? "user" : "assistant") as ChatMessage["role"],
        content: line.text,
        reply: line.reply,
        created_at: "",
        speaker: line.speaker,
        name: line.speaker === "player" ? "You" : line.name,
      }));
    }
    return session?.messages.filter((m) => m.role !== "system") || [];
  }, [group, session]);

  function speakerMeta(message: ChatMessage) {
    if (message.role === "user") return { name: "You", color: "#c9beac" };
    const key = message.speaker;
    const member = group?.definition.members.find(item => item.key === key || item.character.name === message.name);
    const match = characters.find(item => item.id === member?.character_id || item.id === key || item.name === message.name);
    return { name: message.name || character?.name || "Character", color: match?.color || character?.color || "#e2a35a" };
  }

  async function startGroupChat(characterIds: string[], title: string) {
    const created = await api.createGroup(characterIds, title);
    setGroups(prev => [created, ...prev.filter(item => item.id !== created.id)]);
    selectGroup(created.id);
    setCreatingGroup(false);
  }

  if (boot) {
    return (
      <div className="empty">
        <div>
          <h3>LocalTalker</h3>
          <p>{boot}</p><button className="ghost" onClick={() => location.reload()}>Reconnect</button>
        </div>
      </div>
    );
  }

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <h1>LocalTalker</h1>
          <span>Studio</span>
        </div>
        <div className="top-actions">
          <span className="state-pill">
            <i className={`orb ${displayState}`} />
            {connected ? STATE_LABEL[displayState] : "Connecting"}
          </span>
          <button className="ghost" onClick={() => setInspectorOpen((v) => !v)}>
            Inspect
          </button>
          {character && !group && (
            <button className="ghost" onClick={() => setEditor(character)}>
              Edit {character.name}
            </button>
          )}
        </div>
      </header>

      {error && <div className="banner" role="alert">{error}<button onClick={() => setError(null)} aria-label="Dismiss error">?</button></div>}

      <div className={`workspace ${inspectorOpen ? "with-inspector" : ""}`}>
        <aside className="rail">
          <h2>Characters</h2>
          {characters.map((item) => (
            <button
              key={item.id}
              className={`char ${item.id === characterId ? "active" : ""}`}
              onClick={() => selectCharacter(item.id)}
            >
              <span className="avatar" style={{ background: item.color }}>
                {item.name.slice(0, 1)}
              </span>
              <span>
                {item.name}
                <small>{item.provider_id === "mock" ? "Demo character" : item.voice.voice_id.replace(/^[ab][fm]_/, "")}</small>
              </span>
            </button>
          ))}
          <button className="ghost" onClick={() => setCreating(true)}>
            New character
          </button>
          <div className="session-nav"><h2>Group chats</h2>
            <button className="ghost" onClick={() => setCreatingGroup(true)} disabled={characters.length < 2}>New group chat</button>
            {groups.map(item => (
              <button
                key={item.id}
                className={`char ${item.id === groupId ? "active" : ""}`}
                onClick={() => selectGroup(item.id)}
              >
                <span className="avatar-stack">
                  {item.definition.members.slice(0, 3).map((member, index) => {
                    const match = characters.find(c => c.id === member.character_id || c.name === member.character.name);
                    return (
                      <span key={member.key} className="avatar" style={{ background: match?.color || member.character.color, zIndex: 3 - index }}>
                        {(member.character.name || "?").slice(0, 1)}
                      </span>
                    );
                  })}
                </span>
                <span>
                  {item.definition.name}
                  <small>{item.definition.members.map(member => member.character.name).join(", ")}</small>
                </span>
              </button>
            ))}
          </div>
          <div className="session-nav"><h2>Conversations</h2>
            <button className="ghost" onClick={newConversation} disabled={!connected || Boolean(groupId)}>New conversation</button>
            {sessionList.filter(item => !item.game_context?.group_chat).map((item, i) => <button className={`session-link ${item.id === session?.id ? "selected" : ""}`} key={item.id} onClick={async () => { try { openConversation(await api.session(item.id)); } catch(e) {showError(e);} }}>{item.messages[0]?.content?.slice(0, 42) || `Conversation ${sessionList.length - i}`}</button>)}
          </div>
        </aside>

        <main className="stage">
          <div className="character-heading">
            <div>
              <span className="eyebrow">{group ? "GROUP CHAT" : "CONVERSATION"}</span>
              <h2>{group?.definition.name || character?.name || "Choose a character"}</h2>
              {group && (
                <div className="chips" style={{ marginTop: 8 }}>
                  {group.definition.members.map(member => {
                    const match = characters.find(item => item.id === member.character_id || item.name === member.character.name);
                    return <span className="chip" key={member.key} style={{ borderColor: match?.color }}>{member.character.name}</span>;
                  })}
                  <span className="chip">You</span>
                </div>
              )}
            </div>
            <span className="model-badge">{group ? "Multiple characters · take turns" : (character?.provider_id || status?.default_provider_id) === "mock" ? "Demo | simulated replies" : character?.model?.split(/[\\/]/).at(-1)?.replace(/\.gguf$/, "") || "Local inference"}</span>
          </div>
          <div className="transcript" ref={scroller}>
            {!visibleMessages.length && !streaming && (group || character) && (
              <div className="empty">
                <div>
                  <h3>{group?.definition.name || character?.name}</h3>
                  <p>{group
                    ? "Say something and each character can answer in turn. Use the address menu to talk to one person."
                    : (character?.personality || "Speak, type, or inject world context from the inspector.")}</p>
                  {!status?.inference_ready && status?.default_provider_id !== "mock" && <button className="ghost" onClick={() => {setInspectorOpen(true); setTab("setup");}}>Choose a local model</button>}
                </div>
              </div>
            )}
            {visibleMessages.map((message) => {
              const who = speakerMeta(message);
              return (
              <article key={message.id} className={`line ${message.role}`}>
                <div className="who" style={message.role === "assistant" ? { color: who.color } : undefined}>{who.name}</div>
                <div className="said">{message.content}</div>
                {message.reply && <ReplyChips reply={message.reply} />}
              </article>
              );
            })}
            {streaming && (
              <article className="line assistant">
                <div className="who">{streamingName || character?.name}</div>
                <div className="said">{streaming}<span className="caret" /></div>
              </article>
            )}
          </div>

          <div className="composer">
            <div className="composer-shell">
              <textarea
                aria-label="Message"
                value={draft}
                placeholder={group ? (speakTo === "all" ? "Talk to the group…" : `Talk to ${group.definition.members.find(item => item.key === speakTo)?.character.name || "them"}…`) : character ? `Talk to ${character.name}…` : "Choose a character"}
                onChange={(e) => {
                  setDraft(e.target.value);
                  draftRef.current = e.target.value;
                }}
                onKeyDown={(e) => {
                  if (e.key === "Enter" && !e.shiftKey) {
                    e.preventDefault();
                    sendText();
                  }

                }}
              />
              <div className="composer-row">
                <div className="state-pill">
                  <i className={`orb ${state}`} />
                  {armed ? (listenMode === "auto" ? "Listening for a pause" : "Release to send") : listenMode === "auto" ? "Click Mic to listen" : "Hold Mic or Space"}
                  {armed && <meter aria-label="Microphone level" min="0" max="0.2" value={level} />}
                </div>
                <div className="actions">
                  {group && (
                    <select aria-label="Address" value={speakTo} onChange={e => setSpeakTo(e.target.value)}>
                      <option value="all">Everyone</option>
                      {group.definition.members.map(member => (
                        <option key={member.key} value={member.key}>{member.character.name}</option>
                      ))}
                    </select>
                  )}
                  <button className="ghost" aria-pressed={muted} onClick={() => { mutedRef.current = !muted; setMuted(!muted); if (!muted) playback.current.stop(); }}>{muted ? "Unmute" : "Mute"}</button>
                  {busy && (
                    <button className="danger" onClick={interrupt}>
                      Interrupt
                    </button>
                  )}
                  <button
                    className={`mic ${armed ? "hot" : ""}`}
                    disabled={!connected}
                    aria-label="Microphone"
                    onPointerDown={e => { if (listenMode === "push_to_talk") { e.currentTarget.setPointerCapture(e.pointerId); void beginTalk(); } }}
                    onPointerUp={() => listenMode === "push_to_talk" && finishTalk()}
                    onPointerCancel={finishTalk}
                    onClick={() => {
                      if (listenMode !== "auto") return;
                      if (armed) finishTalk();
                      else void beginTalk();
                    }}
                  >
                    Mic
                  </button>
                  <button className="primary" onClick={() => sendText()} disabled={!draft.trim() || !connected || state !== "idle"}>
                    Send
                  </button>
                </div>
              </div>
            </div>
          </div>
        </main>

        <aside className={`inspector ${inspectorOpen ? "open" : ""}`} aria-label="Inspector">
          <div className="tabs">
            {(["reply", "context", "timing", "setup"] as const).map((id) => (
              <button key={id} className={tab === id ? "on" : ""} onClick={() => setTab(id)}>
                {id}
              </button>
            ))}
          </div>

          {tab === "reply" && (
            <div>
              <h2>Structured reply</h2>
              {lastReply ? (
                <>
                  <div className="kv">
                    <span>Emotion</span>
                    <b>{lastReply.emotion || "—"}</b>
                    <span>Intent</span>
                    <b>{lastReply.intent || "—"}</b>
                    <span>Animation</span>
                    <b>{lastReply.animation || "—"}</b>
                  </div>
                  <p className="mono">{JSON.stringify(lastReply, null, 2)}</p>
                </>
              ) : (
                <p className="empty" style={{ height: "auto" }}>
                  Structured fields appear after the first reply.
                </p>
              )}
            </div>
          )}

          {tab === "context" && (
            <div>
              <h2>Game context</h2>
              <div className="field">
                <span>Arbitrary JSON from a host application</span>
                <textarea aria-label="Game context" value={contextText} onChange={(e) => setContextText(e.target.value)} />
              </div>
              <button className="primary" onClick={applyContext}>
                Inject context
              </button>
              <h2 style={{ marginTop: 22 }}>Character state</h2>
              <p className="mono">{JSON.stringify(session?.character_state || {}, null, 2)}</p>
              {session && (
                <button
                  className="ghost"
                  style={{ marginTop: 12 }}
                  disabled={busy}
                  onClick={async () => { try { setSession(await api.clearSession(session.id)); setLastReply(null); setTiming(null); } catch (exc) { showError(exc); } }}
                >
                  Clear history
                </button>
              )}
            </div>
          )}

          {tab === "timing" && (
            <div>
              <h2>Latency</h2>
              {timing ? (
                <div className="kv">
                  {Object.entries(timing.totals_ms).map(([key, value]) => (
                    <FragmentRow key={key} label={key.replace(/_/g, " ")} value={`${value.toFixed(0)} ms`} />
                  ))}
                </div>
              ) : (
                <p>Timings appear after a turn.</p>
              )}
              {firstAudioMs !== null && <p className="mono">Input to playback: {firstAudioMs.toFixed(0)} ms</p>}
              {status?.gpu && (
                <>
                  <h2 style={{ marginTop: 22 }}>GPU</h2>
                  <p className="mono">{JSON.stringify(status.gpu, null, 2)}</p>
                </>
              )}
            </div>
          )}

          {tab === "setup" && (
            <div>
              <h2>Character model</h2>
              {character && <ModelSettings character={character} providers={providers} onSave={async value => { await saveCharacter({...character, ...value}); }} onError={showError} />}
              <details><summary>Provider configuration</summary><ProviderSettings onSaved={refreshStatus} onError={showError} /></details>
              <h2 style={{marginTop: 22}}>Available providers</h2>
              {providers.map((item) => (
                <p key={item.provider.id}>
                  <b>{item.provider.name}</b>{" "}
                  <span className={item.reachable ? "status-ok" : "status-bad"}>
                    {item.reachable ? "reachable" : "offline"}
                  </span>
                  <br />
                  <span className="mono">
                    {item.models.slice(0, 4).join(", ") || item.error || item.provider.base_url}
                  </span>
                </p>
              ))}
              <h2>Audio</h2>
              <p className="mono">{JSON.stringify(status?.speech || {}, null, 2)}</p>
              <div className="field">
                <span>Microphone</span>
                <select aria-label="Microphone device" value={micId} onChange={(e) => setMicId(e.target.value)}>
                  <option value="">System default</option>
                  {devices.inputs.map((d) => (
                    <option key={d.deviceId} value={d.deviceId}>
                      {d.label || d.deviceId}
                    </option>
                  ))}
                </select>
              </div>
              <div className="field">
                <span>Output</span>
                <select aria-label="Output device" value={outId} onChange={(e) => setOutId(e.target.value)}>
                  <option value="">System default</option>
                  {devices.outputs.map((d) => (
                    <option key={d.deviceId} value={d.deviceId}>
                      {d.label || d.deviceId}
                    </option>
                  ))}
                </select>
              </div>
              <div className="field">
                <span>Listen mode</span>
                <select aria-label="Listen mode" value={listenMode} onChange={(e) => setListenMode(e.target.value as "push_to_talk" | "auto")}>
                  <option value="push_to_talk">Push to talk</option>
                  <option value="auto">Voice activity</option>
                </select>
              </div>
              <button className="ghost" onClick={() => void refreshStatus().catch(showError)}>
                Recheck providers
              </button>
            </div>
          )}
        </aside>
      </div>

      {(editor || creating) && (
        <CharacterModal
          character={editor}
          voices={voices}
          providers={providers}
          creating={creating}
          onClose={() => {
            setEditor(null);
            setCreating(false);
          }}
          onSave={async value => { try { if (creating) await createCharacter(value); else if (editor) await saveCharacter({...editor, ...value}); } catch (exc) { showError(exc); throw exc; } }}
          onPreview={(voice, speed) => previewVoice(voice, speed).catch(showError)}
        />
      )}
      {creatingGroup && (
        <GroupChatModal
          characters={characters}
          onClose={() => setCreatingGroup(false)}
          onCreate={async (ids, title) => {
            try { await startGroupChat(ids, title); }
            catch (exc) { showError(exc); throw exc; }
          }}
        />
      )}
    </div>
  );
}

function GroupChatModal({
  characters,
  onClose,
  onCreate,
}: {
  characters: Character[];
  onClose: () => void;
  onCreate: (characterIds: string[], title: string) => Promise<void>;
}) {
  const [picked, setPicked] = useState<string[]>([]);
  const [title, setTitle] = useState("");
  const [saving, setSaving] = useState(false);
  function toggle(id: string) {
    setPicked(prev => prev.includes(id) ? prev.filter(item => item !== id) : [...prev, id]);
  }
  return (
    <div className="modal-back" onClick={onClose}>
      <div className="modal" role="dialog" aria-modal="true" aria-label="New group chat" onClick={e => e.stopPropagation()}>
        <h3>New group chat</h3>
        <p className="hint">Pick at least two characters. You can talk to everyone or address one person.</p>
        <div className="field">
          <span>Title (optional)</span>
          <input aria-label="Group title" value={title} onChange={e => setTitle(e.target.value)} placeholder="Mira, Rook & Ivy" />
        </div>
        <div className="field">
          <span>Characters</span>
          <div className="group-picks">
            {characters.map(item => (
              <label key={item.id} className={`group-pick ${picked.includes(item.id) ? "on" : ""}`}>
                <input type="checkbox" aria-label={item.name} checked={picked.includes(item.id)} onChange={() => toggle(item.id)} />
                <span className="avatar" style={{ background: item.color }}>{item.name.slice(0, 1)}</span>
                {item.name}
              </label>
            ))}
          </div>
        </div>
        <div className="actions">
          <button className="ghost" onClick={onClose}>Cancel</button>
          <button
            className="primary"
            disabled={picked.length < 2 || saving}
            onClick={async () => {
              setSaving(true);
              try { await onCreate(picked, title.trim()); }
              catch { /* parent displays recovery error */ }
              finally { setSaving(false); }
            }}
          >
            {saving ? "Starting…" : "Start group chat"}
          </button>
        </div>
      </div>
    </div>
  );
}

function FragmentRow({ label, value }: { label: string; value: string }) {
  return (
    <>
      <span>{label}</span>
      <b>{value}</b>
    </>
  );
}

function ReplyChips({ reply }: { reply: NonNullable<ChatMessage["reply"]> }) {
  const chips = [
    reply.emotion && `emotion ${reply.emotion}`,
    reply.intent && `intent ${reply.intent}`,
    reply.animation && `anim ${reply.animation}`,
    ...reply.actions.map((a) => a.name),
  ].filter(Boolean) as string[];
  if (!chips.length) return null;
  return (
    <div className="chips">
      {chips.map((chip) => (
        <span className="chip" key={chip}>
          {chip}
        </span>
      ))}
    </div>
  );
}

function CharacterModal({
  character,
  voices,
  providers,
  creating,
  onClose,
  onSave,
  onPreview,
}: {
  character: Character | null;
  voices: VoiceInfo[];
  providers: ProviderStatus[];
  creating: boolean;
  onClose: () => void;
  onSave: (value: Partial<Character>) => Promise<void>;
  onPreview: (voice: string, speed: number) => void;
}) {
  const [name, setName] = useState(character?.name || "");
  const [personality, setPersonality] = useState(character?.personality || "");
  const [instructions, setInstructions] = useState(character?.instructions || "");
  const [voiceId, setVoiceId] = useState(character?.voice.voice_id || "af_bella");
  const [speed, setSpeed] = useState(character?.voice.speed || 1);
  const [saving, setSaving] = useState(false);
  const [providerId, setProviderId] = useState(character?.provider_id || "");
  const [model, setModel] = useState(character?.model || "");

  return (
    <div className="modal-back" onClick={onClose}>
      <div className="modal" role="dialog" aria-modal="true" aria-label={creating ? "New character" : "Edit character"} onClick={(e) => e.stopPropagation()}>
        <h3>{creating ? "New character" : character?.name}</h3>
        <div className="field">
          <span>Name</span>
          <input autoFocus aria-label="Name" value={name} onChange={(e) => setName(e.target.value)} />
        </div>
        <div className="field">
          <span>Personality</span>
          <textarea aria-label="Personality" value={personality} onChange={(e) => setPersonality(e.target.value)} />
        </div>
        <div className="field">
          <span>Instructions</span>
          <textarea aria-label="Instructions" value={instructions} onChange={(e) => setInstructions(e.target.value)} />
        </div>
        <label className="field"><span>Inference provider</span><select aria-label="Provider" value={providerId} onChange={e => {setProviderId(e.target.value); setModel("");}}><option value="">Automatic local provider</option>{providers.map(p => <option key={p.provider.id} value={p.provider.id}>{p.provider.kind === "mock" ? "Demo (simulated replies)" : p.provider.name}</option>)}</select></label>
        <label className="field"><span>Model (blank uses provider default)</span><input value={model} onChange={e => setModel(e.target.value)} /></label>
        <div className="field">
          <span>Voice</span>
          <select aria-label="Voice" value={voiceId} onChange={(e) => setVoiceId(e.target.value)}>
            {voices.map((voice) => (
              <option key={voice.id} value={voice.id}>
                {voice.name} · {voice.traits || voice.id}
              </option>
            ))}
          </select>
        </div>
        <div className="field">
          <span>Speed {speed.toFixed(2)}</span>
          <input type="range" min="0.7" max="1.3" step="0.05" value={speed} onChange={(e) => setSpeed(Number(e.target.value))} />
        </div>
        <div className="actions">
          <button className="ghost" onClick={onClose}>Cancel</button>
          <button className="ghost" onClick={() => onPreview(voiceId, speed)}>
            Preview voice
          </button>
          <button
            className="primary"
            disabled={!name.trim() || saving}
            onClick={async () => {
              setSaving(true);
              try { await onSave({name: name.trim(), personality, instructions, provider_id: providerId || null, model: model || null,
                voice: {voice_id: voiceId, speed, language: voiceId.startsWith("b") ? "en-gb" : "en-us"}}); }
              catch { /* parent displays recovery error */ }
              finally { setSaving(false); }
            }}
          >
            {saving ? "Saving..." : "Save"}
          </button>
        </div>
      </div>
    </div>
  );
}
function ModelSettings({ character, providers, onSave, onError }: { character: Character; providers: ProviderStatus[]; onSave: (v: Partial<Character>) => Promise<void>; onError: (e: unknown) => void }) {
  const [providerId, setProviderId] = useState(character.provider_id || "");
  const [model, setModel] = useState(character.model || "");
  const [saving, setSaving] = useState(false);
  useEffect(() => { setProviderId(character.provider_id || ""); setModel(character.model || ""); }, [character]);
  const models = providers.find(p => p.provider.id === providerId)?.models || [];
  return <div>
    <label className="field"><span>Provider</span><select aria-label="Provider" value={providerId} onChange={e => { setProviderId(e.target.value); setModel(""); }}>
      <option value="">Automatic local provider</option>{providers.map(p => <option key={p.provider.id} value={p.provider.id}>{p.provider.kind === "mock" ? "Demo (simulated replies)" : p.provider.name}</option>)}
    </select></label>
    <label className="field"><span>Model</span><input list="installed-models" value={model} onChange={e => setModel(e.target.value)} placeholder="Provider default" />
      <datalist id="installed-models">{models.map(m => <option key={m} value={m} />)}</datalist>
    </label>
    <button className="primary" disabled={saving} onClick={async () => { setSaving(true); try { await onSave({provider_id: providerId || null, model: model || null}); } catch(e) { onError(e); } finally { setSaving(false); } }}>{saving ? "Saving…" : "Apply model"}</button>
  </div>;
}

function ProviderSettings({ onSaved, onError }: { onSaved: () => Promise<void>; onError: (e: unknown) => void }) {
  const [config, setConfig] = useState<Record<string, any> | null>(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  useEffect(() => { api.getConfig().then(setConfig).catch(onError); }, []);
  if (!config) return <p>Loading configuration…</p>;
  const update = (key: string, value: unknown) => {setSaved(false); setConfig({...config, [key]: value});};
  return <div className="provider-settings">
    <GlimmerSetup onSaved={onSaved} onError={onError} />
    <label className="field"><span>Default provider</span><select value={config.preferred_provider_id} onChange={e => update("preferred_provider_id", e.target.value)}><option value="">Automatic local provider</option>{config.providers.map((p: any) => <option key={p.id} value={p.id}>{p.name}</option>)}</select></label>
    {config.providers.filter((p: any) => p.kind !== "mock").map((p: any, i: number) => <label className="field" key={p.id}><span>{p.name} URL</span><input aria-label={`${p.name} URL`} value={p.base_url} onChange={e => update("providers", config.providers.map((v: any) => v.id === p.id ? {...v, base_url: e.target.value} : v))} /></label>)}
    <label className="field"><span>llama-server executable</span><input value={config.llama_server_path} onChange={e => update("llama_server_path", e.target.value)} placeholder="C:\models\llama-server.exe" /></label>
    <label className="field"><span>GGUF model file</span><input value={config.llama_model_path} onChange={e => update("llama_model_path", e.target.value)} placeholder="C:\models\character.gguf" /></label>
    <label className="field"><span>CUDA 12 / cuDNN library folder (optional)</span><input value={config.speech.cuda_library_path || ""} onChange={e => update("speech", {...config.speech, cuda_library_path:e.target.value})} /></label>
    {config.providers.filter((p: any) => p.kind !== "mock").map((p: any) => <details key={p.id}><summary>{p.name} generation settings</summary>
      <label className="field"><span>Temperature</span><input type="number" min="0" max="2" step="0.1" value={p.temperature} onChange={e => update("providers", config.providers.map((v: any) => v.id === p.id ? {...v, temperature:Number(e.target.value)} : v))} /></label>
      <label className="field"><span>Maximum response tokens</span><input type="number" min="64" max="4096" step="64" value={p.max_tokens} onChange={e => update("providers", config.providers.map((v: any) => v.id === p.id ? {...v, max_tokens:Number(e.target.value)} : v))} /></label>
    </details>)}
    <label className="field"><span>Whisper device</span><select value={config.speech.whisper_device} onChange={e => update("speech", {...config.speech, whisper_device: e.target.value})}><option value="auto">Automatic (CUDA with CPU fallback)</option><option value="cpu">CPU</option><option value="cuda">CUDA</option></select></label>
    <p className="hint">Restart the runtime after changing speech or managed model files.</p>
    <button className="primary" disabled={saving} onClick={async () => {setSaving(true); try {await api.patchConfig(config); await onSaved(); setSaved(true);} catch(e) {onError(e);} finally {setSaving(false);}}}>{saving ? "Saving…" : saved ? "Saved" : "Save configuration"}</button>
  </div>;
}

function GlimmerSetup({ onSaved, onError }: { onSaved: () => Promise<void>; onError: (e: unknown) => void }) {
  const [info, setInfo] = useState<Awaited<ReturnType<typeof api.models>> | null>(null);
  const [busy, setBusy] = useState(false);
  const refresh = useCallback(() => {
    api.models().then(setInfo).catch(onError);
  }, [onError]);
  useEffect(() => {
    refresh();
    const timer = window.setInterval(refresh, 2000);
    return () => window.clearInterval(timer);
  }, [refresh]);
  const glimmer = info?.recommended.find((item) => item.id === "muse-glimmer");
  const job = info?.job;
  const downloading = job?.status === "downloading";
  const pct = glimmer && glimmer.size_bytes
    ? Math.min(100, Math.round(((job?.bytes_on_disk || glimmer.bytes_on_disk || 0) / glimmer.size_bytes) * 100))
    : 0;
  if (!glimmer) return null;
  return (
    <div className="glimmer-card">
      <h3>Muse Glimmer</h3>
      <p>{glimmer.description}</p>
      <p className="mono">
        {glimmer.installed
          ? `Ready · ${glimmer.path}`
          : downloading
            ? `Downloading ${pct}%`
            : `Needs about ${glimmer.min_vram_gb} GB VRAM · ${(glimmer.size_bytes / 1e9).toFixed(1)} GB download`}
      </p>
      <button
        className="primary"
        disabled={busy || downloading}
        onClick={async () => {
          setBusy(true);
          try {
            const result = await api.installModel("muse-glimmer");
            if (result.status === "ready") await onSaved();
            refresh();
          } catch (exc) {
            onError(exc);
          } finally {
            setBusy(false);
          }
        }}
      >
        {glimmer.installed ? "Use Muse Glimmer" : downloading ? `Downloading ${pct}%` : "Install Muse Glimmer"}
      </button>
      {job?.error && <p className="status-bad">{job.error}</p>}
    </div>
  );
}
