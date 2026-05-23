import math
import os

def create_svg(filename, width, height, content):
    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="100%" height="100%">
    <defs>
        <style>
            @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;600&amp;display=swap');
            text {{ font-family: 'Inter', sans-serif; fill: #e0e0e0; font-size: 14px; }}
            .title {{ font-weight: 600; font-size: 16px; fill: #ffffff; }}
            .subtitle {{ font-size: 12px; fill: #a0a0a0; }}
            .box {{ fill: #252526; stroke: #3e3e42; stroke-width: 2; rx: 8; ry: 8; }}
            .box-highlight {{ fill: #2d2d30; stroke: #007acc; stroke-width: 2; rx: 8; ry: 8; }}
            .subgraph {{ fill: rgba(255, 255, 255, 0.02); stroke: #454545; stroke-width: 1; stroke-dasharray: 5,5; rx: 12; ry: 12; }}
            .edge {{ fill: none; stroke: #6b6b6b; stroke-width: 2; stroke-linejoin: round; }}
            .edge-animated {{ fill: none; stroke: #00aaff; stroke-width: 2; stroke-dasharray: 8,8; animation: flow 1s linear infinite; stroke-linejoin: round; }}
            .edge-animated-reverse {{ fill: none; stroke: #00aaff; stroke-width: 2; stroke-dasharray: 8,8; animation: flow-reverse 1s linear infinite; stroke-linejoin: round; }}
            @keyframes flow {{ from {{ stroke-dashoffset: 16; }} to {{ stroke-dashoffset: 0; }} }}
            @keyframes flow-reverse {{ from {{ stroke-dashoffset: 0; }} to {{ stroke-dashoffset: 16; }} }}
            .arrow {{ fill: #6b6b6b; }}
            .arrow-animated {{ fill: #00aaff; }}
            .icon {{ fill: #00aaff; }}
            
            /* Sequence diagram specific */
            .seq-line {{ stroke: #3e3e42; stroke-width: 2; stroke-dasharray: 4,4; }}
            .seq-box {{ fill: #252526; stroke: #007acc; stroke-width: 2; rx: 4; ry: 4; }}
            .seq-msg {{ fill: none; stroke: #00aaff; stroke-width: 2; }}
            .seq-msg-text {{ font-size: 12px; fill: #d4d4d4; }}
            .seq-msg-anim {{ stroke-dasharray: 1000; stroke-dashoffset: 1000; animation: draw 2s ease forwards infinite; }}
            @keyframes draw {{ to {{ stroke-dashoffset: 0; }} }}
            
            /* Architecture specific */
            .arch-group {{ fill: #1e1e1e; stroke: #333; stroke-width: 2; rx: 10; ry: 10; }}
            .arch-group-title {{ font-weight: 600; font-size: 18px; fill: #888; }}
            .arch-node {{ fill: #252526; stroke: #555; stroke-width: 2; rx: 6; ry: 6; transition: all 0.3s ease; }}
            .arch-node:hover {{ stroke: #00aaff; transform: translateY(-2px); }}
        </style>
        <marker id="arrowhead" markerWidth="10" markerHeight="7" refX="0" refY="3.5" orient="auto">
            <polygon points="0 0, 10 3.5, 0 7" class="arrow" />
        </marker>
        <marker id="arrowhead-anim" markerWidth="10" markerHeight="7" refX="0" refY="3.5" orient="auto">
            <polygon points="0 0, 10 3.5, 0 7" class="arrow-animated" />
        </marker>
    </defs>
    <rect width="100%" height="100%" fill="#181818" rx="12" ry="12" />
    {content}
</svg>"""
    with open(filename, "w", encoding="utf-8") as f:
        f.write(svg)

def draw_box(x, y, w, h, title, subtitle="", icon="", highlight=False):
    cls = "box-highlight" if highlight else "box"
    res = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" class="{cls}" />\n'
    
    text_y = y + 25 if subtitle else y + h/2 + 5
    if icon:
        # Simple icon placeholder
        res += f'<circle cx="{x + 25}" cy="{y + 25}" r="12" fill="#333" />\n'
        res += f'<text x="{x + 25}" y="{y + 30}" text-anchor="middle" class="icon" font-size="14">{icon}</text>\n'
        text_x = x + 45
    else:
        text_x = x + w/2
        
    anchor = "start" if icon else "middle"
    
    res += f'<text x="{text_x}" y="{text_y}" text-anchor="{anchor}" class="title">{title}</text>\n'
    if subtitle:
        lines = subtitle.split("<br/>")
        for i, line in enumerate(lines):
            res += f'<text x="{text_x}" y="{text_y + 20 + i*16}" text-anchor="{anchor}" class="subtitle">{line}</text>\n'
    return res

def draw_edge(x1, y1, x2, y2, animated=True, reverse=False, curve="horizontal"):
    cls = "edge-animated" if animated else "edge"
    if reverse: cls = "edge-animated-reverse"
    marker = "url(#arrowhead-anim)" if animated else "url(#arrowhead)"
    
    x2_short, y2_short = x2, y2
    if curve == "horizontal":
        if x2 > x1: x2_short -= 10
        else: x2_short += 10
        cx1, cy1 = x1 + (x2_short - x1)/2, y1
        cx2, cy2 = x2_short - (x2_short - x1)/2, y2_short
    else:
        if y2 > y1: y2_short -= 10
        else: y2_short += 10
        cx1, cy1 = x1, y1 + (y2_short - y1)/2
        cx2, cy2 = x2_short, y2_short - (y2_short - y1)/2
        
    return f'<path d="M {x1} {y1} C {cx1} {cy1}, {cx2} {cy2}, {x2_short} {y2_short}" class="{cls}" marker-end="{marker}" fill="none" />\n'

def draw_straight_edge(x1, y1, x2, y2, animated=True):
    cls = "edge-animated" if animated else "edge"
    marker = "url(#arrowhead-anim)" if animated else "url(#arrowhead)"
    
    length = math.hypot(x2 - x1, y2 - y1)
    if length > 10:
        dx = (x2 - x1) / length * 10
        dy = (y2 - y1) / length * 10
        x2_short = x2 - dx
        y2_short = y2 - dy
    else:
        x2_short, y2_short = x2, y2
        
    return f'<line x1="{x1}" y1="{y1}" x2="{x2_short}" y2="{y2_short}" class="{cls}" marker-end="{marker}" />\n'

def generate_system_glance():
    content = ""
    # Subgraph LocalTalker
    content += '<rect x="470" y="40" width="730" height="360" class="subgraph" />\n'
    content += '<text x="485" y="65" class="subtitle">LocalTalker plugin</text>\n'
    
    # Nodes
    content += draw_box(30, 80, 150, 60, "Player", "microphone", "🎤")
    content += draw_box(30, 320, 150, 60, "Nearby NPCs", "", "🤖")
    
    content += draw_box(210, 80, 250, 60, "Voice Input", "AutoChatVoiceInputComponent", "🎙️")
    content += draw_box(210, 180, 250, 60, "Whisper STT", "LocalPlayerInteractionComponent", "📝")
    content += draw_box(210, 280, 250, 60, "Conversation History", "auto replies + keep-alive", "📚")
    
    content += draw_box(490, 180, 250, 60, "Director", "LocalTalkConversationSubsystem", "🎬", True)
    content += draw_box(490, 320, 250, 60, "Audio", "Procedural audio + subtitles", "🔈")
    
    content += draw_box(770, 180, 210, 60, "Character", "LocalCharacterComponent", "👤")
    
    content += draw_box(1010, 80, 170, 60, "llama.cpp", "local GGUF model", "🧠")
    content += draw_box(1010, 280, 170, 60, "Kokoro TTS", "TTS worker", "🔊")
    
    # Edges
    # Player -> Voice
    content += draw_straight_edge(180, 110, 210, 110)
    # Voice -> Whisper
    content += draw_straight_edge(335, 140, 335, 180)
    # Whisper -> Director
    content += draw_straight_edge(460, 210, 490, 210)
    # Director -> Character
    content += draw_straight_edge(740, 210, 770, 210)
    
    # Character -> llama
    content += draw_edge(980, 195, 1010, 110, curve="horizontal")
    # llama -> Character
    content += draw_edge(1010, 130, 980, 215, curve="horizontal")
    
    # Character -> TTS
    content += draw_edge(980, 225, 1010, 310, curve="horizontal")
    # TTS -> Audio
    content += draw_edge(1010, 325, 740, 350, curve="horizontal")
    
    # Audio -> NPCs
    content += draw_straight_edge(490, 350, 180, 350)
    
    # NPCs -> Director
    # Route up through gap 1 (x=195), across gap 2 (y=260), up gap 3 (x=475)
    content += f'<path d="M 180 335 L 195 335 L 195 260 L 475 260 L 475 225 L 480 225" class="edge-animated" marker-end="url(#arrowhead-anim)" fill="none" />\n'
    
    # Director -> History
    content += draw_edge(490, 225, 460, 295, curve="horizontal")
    
    # History -> Character
    content += draw_edge(460, 310, 770, 225, curve="horizontal")
    
    create_svg("docs/readme/system-at-a-glance.svg", 1220, 450, content)

def generate_sequence():
    content = ""
    # Participants
    participants = [
        ("Player", 100),
        ("Voice Input", 290),
        ("Whisper STT", 480),
        ("Director", 670),
        ("Character", 860),
        ("llama.cpp", 1050),
        ("Kokoro TTS", 1240)
    ]
    
    for name, x in participants:
        content += f'<rect x="{x-60}" y="40" width="120" height="40" class="seq-box" />\n'
        content += f'<text x="{x}" y="65" text-anchor="middle" class="title">{name}</text>\n'
        content += f'<line x1="{x}" y1="80" x2="{x}" y2="600" class="seq-line" />\n'
        
    y = 120
    def msg(p1, p2, text, reverse=False, dashed=False):
        nonlocal y, content
        x1 = participants[p1][1]
        x2 = participants[p2][1]
        
        cls = "seq-msg"
        if dashed: cls += " seq-line"
        
        marker = "url(#arrowhead-anim)"
        
        # Add a subtle animation delay based on y position
        anim_delay = (y - 120) / 100
        style = f"animation-delay: {anim_delay}s;"
        
        if p1 == p2:
            # Self message loop
            content += f'<path d="M {x1} {y} L {x1+40} {y} L {x1+40} {y+25} L {x1+10} {y+25}" class="{cls}" marker-end="{marker}" style="fill:none; {style}" />\n'
            content += f'<text x="{x1+45}" y="{y+15}" text-anchor="start" class="seq-msg-text">{text}</text>\n'
            y += 45
        else:
            x2_short = x2 - 10 if x2 > x1 else x2 + 10
            content += f'<line x1="{x1}" y1="{y}" x2="{x2_short}" y2="{y}" class="{cls}" marker-end="{marker}" style="{style}" />\n'
            text_x = (x1 + x2) / 2
            content += f'<text x="{text_x}" y="{y-10}" text-anchor="middle" class="seq-msg-text">{text}</text>\n'
            y += 40

    msg(0, 1, "Speaks near a booth")
    msg(1, 1, "Detect speech, suppress noise")
    msg(1, 2, "Submit captured PCM audio")
    msg(2, 1, "Transcript", dashed=True)
    msg(1, 3, "Route recognized speech")
    msg(3, 3, "Record user line, prioritize")
    msg(3, 4, "Grant turn")
    msg(4, 5, "Build prompt from persona")
    msg(5, 4, "Stream generated text", dashed=True)
    msg(4, 6, "Chunk sentences for synthesis")
    msg(6, 4, "Speech audio", dashed=True)
    msg(4, 0, "Play voice + subtitles")
    msg(4, 3, "Broadcast spoken line")
    msg(3, 3, "Pick next speaker or pause")
    
    create_svg("docs/readme/live-conversation-flow.svg", 1350, 650, content)

def generate_architecture():
    content = ""
    
    # Groups
    content += '<rect x="30" y="40" width="320" height="300" class="arch-group" />\n'
    content += '<text x="190" y="70" text-anchor="middle" class="arch-group-title">AutoChat game module</text>\n'
    
    content += '<rect x="390" y="40" width="340" height="340" class="arch-group" />\n'
    content += '<text x="560" y="70" text-anchor="middle" class="arch-group-title">LocalTalker plugin</text>\n'
    
    content += '<rect x="760" y="40" width="300" height="300" class="arch-group" />\n'
    content += '<text x="910" y="70" text-anchor="middle" class="arch-group-title">Local AI runtimes</text>\n'
    
    # Nodes
    content += draw_box(50, 100, 280, 50, "AutoChatVoiceInputComponent")
    content += draw_box(50, 180, 280, 50, "ConventionBotSubsystem")
    content += draw_box(50, 260, 280, 50, "Convention expo scene")
    
    content += draw_box(420, 100, 280, 50, "LocalPlayerInteractionComponent")
    content += draw_box(420, 170, 280, 50, "LocalTalkConversationSubsystem")
    content += draw_box(420, 240, 280, 50, "LocalCharacterComponent")
    content += draw_box(420, 310, 280, 50, "LocalTalkerSettings")
    
    content += draw_box(790, 100, 240, 50, "Whisper / faster-whisper")
    content += draw_box(790, 180, 240, 50, "llama.cpp")
    content += draw_box(790, 260, 240, 50, "Kokoro ONNX TTS")
    
    # Edges
    content += draw_straight_edge(330, 125, 420, 125) # VoiceInput -> PlayerBridge
    content += draw_straight_edge(700, 125, 790, 125) # PlayerBridge -> Whisper
    content += draw_edge(330, 125, 420, 195, curve="horizontal") # VoiceInput -> Conversation
    content += draw_straight_edge(560, 220, 560, 240) # Conversation -> Character
    content += draw_edge(700, 255, 790, 205, curve="horizontal") # Character -> Llama
    content += draw_edge(700, 275, 790, 285, curve="horizontal") # Character -> Kokoro
    content += draw_straight_edge(560, 310, 560, 290) # Settings -> Character
    
    # Settings -> PlayerBridge (route around left side)
    content += f'<path d="M 420 335 C 360 335, 360 125, 410 125" class="edge-animated" marker-end="url(#arrowhead-anim)" fill="none" />\n'
    
    content += draw_straight_edge(190, 230, 190, 260) # BotDirector -> DemoScene
    content += draw_edge(330, 205, 420, 265, curve="horizontal") # BotDirector -> Character
    
    create_svg("docs/readme/core-architecture.svg", 1100, 390, content)

if __name__ == "__main__":
    os.makedirs("docs/readme", exist_ok=True)
    generate_system_glance()
    generate_sequence()
    generate_architecture()
    print("SVGs generated successfully.")
