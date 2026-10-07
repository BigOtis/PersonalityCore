# Human-authored characters, dynamic interaction

**PersonalityCore** (formerly LocalTalker) gives an authored character flexibility
when the player asks an unexpected question or requests an action in the world.
Human developers and writers define the experience: the script, character,
voice, canon, story beats, puzzle rules and permitted behavior.

The intended game workflow combines authored dialogue with dynamic responses.
Use fixed lines when the wording matters. Use model-generated speech to clarify
a reference, answer from supplied facts, acknowledge a player request or react
to a confirmed event. The host decides which mode applies and which actions
can occur. Open studio/group chat remains useful for exploring a personality;
it is not a requirement to generate a game's whole dialogue or story.

## Authored speech uses no dialogue inference

The scene API already supports `speak`: exact host-authored text, synthesized
with the chosen character voice and recorded in scene history. It bypasses
language-model generation. The targeted member must exist in the scene.

```json
{
  "type": "speak",
  "target": "guide",
  "text": "The next assessment begins when you are ready."
}
```

Send this over `/v1/scenes/{scene_id}/live`, or post it to the scene commands
endpoint. It produces the normal voice/turn events; acknowledge playback with
the matching `turn_id`. It interrupts the current scene turn, so your host should
schedule authored speech deliberately rather than competing with player input.
An Unreal host can use `SendCommandJson`; no game-specific plugin is required.
Recorded dialogue and sound assets can also be played directly by the host.

## Dynamic replies stay grounded in the authored scene

Character instructions, private backstory, public descriptions, relationships,
scene goals and guidance supply the authored direction. Current context provides
canonical facts, the exact request, relevant object identities and actual state.
Companion hosts offer only the actions relevant to this instruction, retaining
required prerequisites. They preserve ambiguity instead of making up a target.

For example, "put that can on the table" uses a looked-at object ID and an
accepted pickup/place plan. "Tell me about that notice" uses authored readable
text. A blocked route produces an explicit failure rather than a fictional
arrival. The author controls the records and the puzzle; the player can still
interact using words the author did not enumerate in a dialogue menu.

The host validates action names, targets, prerequisites, reachability and script
state before executing a proposal. It advances story or assessment state from
confirmed outcomes. Personality guidance alone does not enforce game rules or
guarantee every generated word: consequential boundaries belong in host code.

## Reusable across hosts

- **Studio:** author/prototype personality, instructions, voices and responses.
- **UE5 plugin:** transport player requests, voice and proposed actions; your
  game owns authored dialogue scheduling and gameplay execution.
- **Three.js:** the same runtime contract, with browser input, rendering and
  audio presentation. The included example has no gameplay executor.
- **Cohersion:** an integration case study combining authored records,
  announcements, reactions and assessments with flexible COLIN requests.

The product name is PersonalityCore. Existing `localtalker` packages, URLs,
environment variables, data folders and Unreal class/module identifiers remain
valid so the rebrand does not require migrating integrations or user data.
