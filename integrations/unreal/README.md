# PersonalityCore for Unreal Engine

Copy the `LocalTalker` folder beside this file into your project's `Plugins` directory, enable the plugin, and rebuild. The plugin was developed against UE 5.8. It contains no demo map, game module, or game assets.

Run the Windows studio or standalone PersonalityCore runtime separately. In **Project Settings → Game → PersonalityCore**, set the runtime URL (normally `http://127.0.0.1:8765`).

1. Add a **PersonalityCore category / LocalTalker Character** component to each participating actor and match its `CharacterKey` to a member key in your scene JSON.
2. Get the **PersonalityCore category / LocalTalker Subsystem** from the game instance and call `OpenSceneJson` with your scene definition.
3. Call `SetPlayerPresence` from your proximity logic, or `JoinConversation` for explicit invitations.
4. Connect pressed/released input to `BeginPushToTalk` and `EndPushToTalk(Target)`. Use `SendText` for typed input.
5. Bind `OnSceneEvent` to consume structured replies. Validate and execute only actions supported by your game.
6. Use `SetDirection` for scene goals and context, and `ContinueConversation` for bounded character exchanges.

The plugin handles microphone capture, streamed PCM playback, and scene playback acknowledgement. Methods are available in Blueprints and C++. `SubmitPCM16` accepts audio from a custom capture system.

The host owns movement, animation, interaction, authoritative memory, and multiplayer authority. The plugin connects to an external runtime; copying it alone does not bundle Python, speech models, or language-model weights into a game build.

Read [scene definitions and playback scheduling](../../docs/scenes.md) and [the integration contract](../../docs/integration.md).

The display brand is PersonalityCore. The `LocalTalker` plugin folder, module and C++ class identifiers remain stable for existing projects. [Authored characters](../../docs/authored-characters.md) explains exact scripted speech and flexible responses to player requests. [Cohersion](../../docs/cohersion.md) is an integration case study, not a dependency.
