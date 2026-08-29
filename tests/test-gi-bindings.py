#!/usr/bin/env python3
"""
test-gi-bindings.py -- end-to-end PyGObject smoke test for ai-glib

Validates that the .typelib produced by `make GIR=1` is actually usable
from Python: constructors work, properties round-trip, enums are typed,
new helpers (_with_name, register_callback, etc.) are exposed correctly.

Run via `make test-gi`, or directly:

    LD_LIBRARY_PATH=build/release \\
    GI_TYPELIB_PATH=build/release \\
        /usr/bin/python3 tests/test-gi-bindings.py

Requires python3-gobject (Fedora) / python3-gi (Debian).
"""
import os
import sys
import tempfile


def main():
    try:
        import gi
    except ImportError:
        print("SKIP: python3-gobject not installed", file=sys.stderr)
        return 0

    gi.require_version("AiGlib", "1.0")
    from gi.repository import AiGlib, Gio  # noqa: E402

    # ----- Class construction -----
    client = AiGlib.ClaudeClient.new()
    assert isinstance(client, AiGlib.ClaudeClient)
    assert isinstance(client, AiGlib.Client)  # inheritance visible

    # ----- Property round-trip -----
    client.props.model = "claude-sonnet-4-5"
    assert client.props.model == "claude-sonnet-4-5", f"got {client.props.model!r}"
    client.props.max_tokens = 1024
    assert client.props.max_tokens == 1024
    client.props.temperature = 0.7
    assert abs(client.props.temperature - 0.7) < 1e-6
    client.props.system_prompt = "You are helpful."
    assert client.props.system_prompt == "You are helpful."

    # Native method form still works
    assert client.get_model() == "claude-sonnet-4-5"
    client.set_model("claude-opus-4-5")
    assert client.props.model == "claude-opus-4-5"

    # ----- AiConfig property -----
    cfg = AiGlib.Config.new()
    cfg.props.timeout = 90
    assert cfg.props.timeout == 90

    # ----- AiMessage constructors -----
    user_msg = AiGlib.Message.new_user("Hello, world!")
    assert user_msg.get_role() == AiGlib.Role.USER
    assert user_msg.get_text() == "Hello, world!"

    assistant_msg = AiGlib.Message.new_assistant("Hi there.")
    assert assistant_msg.get_role() == AiGlib.Role.ASSISTANT

    # The new constructor added with the HTTP tool-calling fix
    result_msg = AiGlib.Message.new_tool_result_with_name(
        "call_1", "get_weather", "sunny, 82F", False
    )
    assert result_msg.get_role() == AiGlib.Role.USER
    blocks = result_msg.get_content_blocks()
    assert len(blocks) == 1
    tr = blocks[0]
    assert isinstance(tr, AiGlib.ToolResult)
    assert tr.get_tool_use_id() == "call_1"
    assert tr.get_tool_name() == "get_weather"
    assert tr.get_content() == "sunny, 82F"
    assert tr.get_is_error() is False

    # ----- AiTool definition -----
    tool = AiGlib.Tool.new("get_weather", "Look up the weather")
    tool.add_parameter("city", "string", "City name", True)
    assert tool.get_name() == "get_weather"

    # ----- Enum round-trip (string <-> enum) -----
    assert AiGlib.role_from_string("user") == AiGlib.Role.USER
    assert AiGlib.role_to_string(AiGlib.Role.ASSISTANT) == "assistant"

    # ----- AiToolUse -----
    tu = AiGlib.ToolUse.new_from_json_string(
        "call_x", "calc", '{"a":13,"b":7}'
    )
    assert tu.get_id() == "call_x"
    assert tu.get_name() == "calc"
    assert tu.get_input_int("a", 0) == 13

    # ----- AiToolExecutor + register_callback (the callback that needed
    #       the scope/closure annotation fix) -----
    state = {"calls": 0, "last_query": None}

    def my_lookup(tool_use, cancellable, user_data):
        state["calls"] += 1
        state["last_query"] = tool_use.get_input_string("query")
        return f"looked up: {state['last_query']}"

    exec_ = AiGlib.ToolExecutor.new()
    custom_tool = AiGlib.Tool.new("my_lookup", "look something up")
    custom_tool.add_parameter("query", "string", "what to look up", True)
    exec_.register_callback(custom_tool, my_lookup, None)

    # Drive the callback by hand via the public execute() entry point.
    request = AiGlib.ToolUse.new_from_json_string(
        "call_42", "my_lookup", '{"query": "weather"}'
    )
    result = exec_.execute(request, None)
    assert result == "looked up: weather", f"got {result!r}"
    assert state["calls"] == 1
    assert state["last_query"] == "weather"

    # ----- CLI providers: AiGrokBuildClient -----
    # Every knob on this one is a real GObject property specifically so
    # bindings get native syntax, so exercise that path rather than the
    # C accessors.
    grok = AiGlib.GrokBuildClient.new()
    assert isinstance(grok, AiGlib.GrokBuildClient)
    assert isinstance(grok, AiGlib.CliClient)  # inheritance visible
    assert grok.props.model == "grok-4.6"

    grok.props.model = "grok-4.5"
    grok.props.effort_level = "xhigh"
    grok.props.skip_permissions = True
    grok.props.allowed_tools = "read_file,list_dir"
    grok.props.max_turns = 5
    grok.props.sandbox = "workspace"
    assert grok.props.model == "grok-4.5"
    assert grok.props.effort_level == "xhigh"
    assert grok.props.skip_permissions is True
    assert grok.props.allowed_tools == "read_file,list_dir"
    assert grok.props.max_turns == 5
    assert grok.props.sandbox == "workspace"

    # verbatim defaults on; total-cost is read-only and starts at zero
    assert grok.props.verbatim is True
    assert grok.props.total_cost == 0.0

    # The interfaces the provider claims are visible to introspection
    assert isinstance(grok, AiGlib.Provider)
    assert isinstance(grok, AiGlib.Streamable)
    assert grok.get_provider_type() == AiGlib.ProviderType.GROK_BUILD
    assert grok.get_name() == "Grok Build"
    assert grok.get_default_model() == "grok-4.6"

    # Provider name round-trip, including the one that must not collide
    # with the HTTP "grok" provider
    assert AiGlib.provider_type_from_string("grok-build") == \
        AiGlib.ProviderType.GROK_BUILD
    assert AiGlib.provider_type_from_string("grok") == AiGlib.ProviderType.GROK
    assert AiGlib.provider_type_to_string(
        AiGlib.ProviderType.GROK_BUILD) == "grok-build"

    agy = AiGlib.AntigravityClient.new()
    assert isinstance(agy, AiGlib.AntigravityClient)
    assert isinstance(agy, AiGlib.CliClient)
    assert agy.props.model == "gemini-3.7-flash-high"
    agy.props.skip_permissions = True
    agy.props.mode = "plan"
    agy.props.sandbox = True
    assert agy.props.skip_permissions is True
    assert agy.props.mode == "plan"
    assert agy.props.sandbox is True
    assert agy.props.disable_slash_commands is True
    assert isinstance(agy, AiGlib.Provider)
    assert isinstance(agy, AiGlib.Streamable)
    assert agy.get_provider_type() == AiGlib.ProviderType.ANTIGRAVITY
    assert agy.get_name() == "Antigravity"
    assert AiGlib.provider_type_from_string("antigravity") == \
        AiGlib.ProviderType.ANTIGRAVITY
    assert AiGlib.provider_type_from_string("agy") == \
        AiGlib.ProviderType.ANTIGRAVITY
    assert AiGlib.provider_type_to_string(
        AiGlib.ProviderType.ANTIGRAVITY) == "antigravity"

    cursor = AiGlib.CursorClient.new()
    assert isinstance(cursor, AiGlib.CursorClient)
    assert isinstance(cursor, AiGlib.CliClient)
    assert cursor.props.model == "auto"
    cursor.props.skip_permissions = True
    cursor.props.mode = "plan"
    cursor.props.sandbox = "disabled"
    cursor.props.trust = True
    assert cursor.props.skip_permissions is True
    assert cursor.props.mode == "plan"
    assert cursor.props.sandbox == "disabled"
    assert cursor.props.trust is True
    assert isinstance(cursor, AiGlib.Provider)
    assert isinstance(cursor, AiGlib.Streamable)
    assert cursor.get_provider_type() == AiGlib.ProviderType.CURSOR
    assert cursor.get_name() == "Cursor"
    assert AiGlib.provider_type_from_string("cursor") == \
        AiGlib.ProviderType.CURSOR
    assert AiGlib.provider_type_from_string("cursor-agent") == \
        AiGlib.ProviderType.CURSOR
    assert AiGlib.provider_type_to_string(
        AiGlib.ProviderType.CURSOR) == "cursor"

    # ----- Boxed type roundtrip -----
    usage = AiGlib.Usage.new(100, 50)
    assert usage.get_input_tokens() == 100
    assert usage.get_output_tokens() == 50
    assert usage.get_total_tokens() == 150

    # ----- The view layer, which is the Emacs path -----
    #
    # An Emacs front-end reaches all of this through introspection, exactly
    # as PyGObject does here. If a signature stops surviving the scanner,
    # this is where it shows up rather than in somebody's init.el.

    transcript = AiGlib.Transcript.new()
    assert isinstance(transcript, Gio.ListModel)
    assert transcript.get_n_items() == 0

    seen = {"added": 0, "changed": 0}

    def on_items_changed(model, position, removed, added):
        seen["added"] += added

    def on_block_changed(t, position, block):
        seen["changed"] += 1

    transcript.connect("items-changed", on_items_changed)
    transcript.connect("block-changed", on_block_changed)

    turn = AiGlib.ViewTurnBlock.new("hello from python")
    transcript.append(turn)
    assert transcript.get_n_blocks() == 1
    assert seen["added"] == 1
    assert turn.get_kind() == AiGlib.ViewBlockKind.TURN
    assert turn.get_id() > 0

    # A block mutating in place is neither an insertion nor a removal, and
    # ::block-changed is what tells a frontend to update one region.
    text_block = AiGlib.ViewTextBlock.new()
    transcript.append(text_block)
    text_block.append("streamed ")
    text_block.append("in pieces")
    assert seen["changed"] >= 2

    # ----- Rendering, and reading the spans back -----
    #
    # The out-parameter accessor exists precisely so this works: a GArray of
    # a plain struct would not survive the scanner.
    rendered = turn.render(0)
    assert rendered.get_text() == "> hello from python"
    assert rendered.get_n_spans() >= 1

    ok, start, length, tag = rendered.get_span(0)
    assert ok
    assert length > 0
    assert start + length <= rendered.get_length()

    # Offsets are byte offsets, which is what byte-to-position converts in
    # Emacs. Python slices bytes the same way.
    raw = rendered.get_text().encode("utf-8")
    assert len(raw[start:start + length]) == length

    # Style names are the frontend's mapping key -- ai-glib-face-<name>.
    assert AiGlib.style_tag_to_string(AiGlib.StyleTag.TOOL_NAME) == "tool-name"
    assert AiGlib.style_tag_from_string("tool-name") == AiGlib.StyleTag.TOOL_NAME

    # ----- The tool summary, from bindings -----
    tool_block = AiGlib.ViewToolBlock.new()
    for i, (name, args) in enumerate(
            [("Write", '{"file_path": "a.c", "content": "x\\ny"}'),
             ("Write", '{"file_path": "b.c", "content": "p"}')]):
        tu = AiGlib.ToolUse.new_from_json_string(f"t{i}", name, args)
        tool_block.add_call(tu)

    assert tool_block.get_n_calls() == 2
    assert tool_block.get_summary() == "Created 2 files  +3-0"

    # ----- A conversation, without sending anything -----
    conversation = AiGlib.Conversation.new(grok)
    assert conversation.get_transcript() is not None
    assert conversation.get_busy() is False
    conversation.set_system_prompt("be brief")
    assert conversation.get_system_prompt() == "be brief"

    # Idle: no activity, no elapsed. An Emacs frontend draws its own
    # spinner and takes the words from here.
    assert conversation.get_activity() is None
    assert conversation.get_activity_elapsed() == 0

    # local-tools is refused for a CLI wrapper, which runs its own.
    conversation.set_local_tools(True)
    assert conversation.get_local_tools() is False

    # ----- Events -----
    event = AiGlib.Event.new_text_delta("hello")
    assert event.get_kind() == AiGlib.EventKind.TEXT_DELTA
    assert event.get_text() == "hello"
    assert event.get_cost_micros() == -1
    assert AiGlib.event_kind_to_string(AiGlib.EventKind.TOOL_STARTED) == \
        "tool-started"
    assert isinstance(grok, AiGlib.EventSource)

    # ----- The harness layer -----
    #
    # This section is the standing check that an Emacs frontend can drive
    # the input pipeline: everything below goes through introspection, and
    # if any of it stops being reachable from Python it has stopped being
    # reachable from elisp too.
    with tempfile.TemporaryDirectory() as box:
        os.makedirs(os.path.join(box, ".claude", "commands"))
        with open(os.path.join(box, ".claude", "commands", "deploy.md"),
                  "w") as fp:
            fp.write("---\ndescription: Ship it\n"
                     "argument-hint: <env>\n---\nDeploy to $1.\n")
        with open(os.path.join(box, "hello.c"), "w") as fp:
            fp.write("int main(void) { return 0; }\n")

        registry = AiGlib.ResourceRegistry.new()
        registry.set_working_directory(box)

        # The scan reads $HOME too, which this process cannot redirect
        # after GLib has cached it -- so assert only on the project file.
        registry.scan()

        resource = registry.lookup(AiGlib.ResourceKind.COMMAND, "deploy")
        assert resource is not None
        assert resource.get_description() == "Ship it"
        assert resource.get_origin() == "claude"
        assert resource.get_meta("argument-hint") == "<env>"

        commands = AiGlib.CommandSet.new(registry)
        resolved = commands.resolve("/deploy staging", box, None)
        assert resolved.get_outcome() == AiGlib.CommandOutcome.PROMPT
        assert resolved.get_prompt() == "Deploy to staging.\n"

        # A built-in comes back for the frontend to act on.
        builtin = commands.resolve("/clear", box, None)
        assert builtin.get_outcome() == AiGlib.CommandOutcome.BUILTIN
        assert builtin.get_name() == "clear"

        # ----- Mentions -----
        mentions = AiGlib.mention_scan("explain @hello.c please")
        assert len(mentions) == 1
        assert mentions[0].path == "hello.c"
        # Byte offsets, the same convention AiStyleSpan uses.
        assert mentions[0].start == 8

        expanded = AiGlib.mention_expand("explain @hello.c", box, 0)
        assert "int main(void)" in expanded[0]

        # ----- Completion: the range is what completion-at-point wants -----
        ctx = AiGlib.CompletionContext.new(commands, box)

        result = ctx.query("@hel", 4)
        assert result.get_kind() == AiGlib.CompletionKind.PATH
        assert result.get_start() == 1
        assert result.get_end() == 4
        assert result.get_n_items() == 1

        ok, text, display, description, origin, is_dir = \
            result.get_item_fields(0)
        assert ok is True
        assert text == "hello.c"
        assert is_dir is False
        assert origin is None

        result = ctx.query("/dep", 4)
        assert result.get_kind() == AiGlib.CompletionKind.COMMAND
        assert result.get_start() == 1
        assert result.get_common_prefix() == "deploy"

    # ----- Todos -----
    todo = AiGlib.Todo.new("Add the parser", "Adding the parser",
                           AiGlib.TodoState.IN_PROGRESS)
    assert todo.get_label() == "Adding the parser"
    assert AiGlib.todo_state_to_string(AiGlib.TodoState.COMPLETED) == \
        "completed"

    todo_block = AiGlib.ViewTodoBlock.new()
    assert todo_block.get_n_todos() == 0
    assert todo_block.get_kind() == AiGlib.ViewBlockKind.TODO

    # ----- Background agents -----
    #
    # The same standing check for the agent layer: an Emacs frontend
    # spawns, watches and kills background agents through exactly these
    # calls. Nothing here contacts a provider -- the agent is never
    # started -- so this stays offline like the rest of the file.
    brigade = AiGlib.Brigade.new()
    worker = AiGlib.LocalWorker.new()
    brigade.set_worker(worker)
    brigade.set_max_concurrent(2)
    assert brigade.get_max_concurrent() == 2
    assert brigade.get_worker() is not None

    # Ids are minted by the brigade, so a frontend need not invent them.
    agent_id = brigade.generate_id("reviewer")
    assert agent_id.startswith("reviewer-")

    agent = AiGlib.Agent.new(agent_id, None)
    agent.set_description("read the diff")
    assert brigade.add(agent) is True
    assert brigade.get(agent_id) is agent
    assert len(brigade.list()) == 1
    assert brigade.count_live() == 0

    # AiAgent owns the ai_agent_ prefix, so the scanner files these under
    # the class rather than at namespace level. Asserted in the shape a
    # binding actually sees, which is the shape elisp will use.
    assert AiGlib.Agent.state_to_string(AiGlib.AgentState.RUNNING) == "running"
    assert AiGlib.Agent.state_is_live(AiGlib.AgentState.RUNNING) is True
    assert AiGlib.Agent.state_is_terminal(AiGlib.AgentState.DONE) is True

    # Cancelling is always allowed, and the state says what happened.
    agent.cancel()
    assert agent.get_state() == AiGlib.AgentState.CANCELLED

    # A finish is remembered until it is collected -- the mechanism a
    # frontend polls, and the one the model is notified through.
    assert brigade.take_finished() == agent_id
    assert brigade.take_finished() is None

    # Reaping returns whatever it produced and forgets the agent.
    reaped = brigade.reap(agent_id)
    assert reaped is not None
    assert brigade.get(agent_id) is None

    # The panel renders from live agents and keeps its rows afterwards.
    agent_block = AiGlib.ViewAgentBlock.new()
    assert agent_block.get_kind() == AiGlib.ViewBlockKind.AGENT
    assert agent_block.get_n_agents() == 0
    agent_block.set_agents([agent])
    assert agent_block.get_n_agents() == 1
    assert agent_block.get_n_live() == 0
    assert "read the diff" in agent_block.render(0).get_text()

    # And a conversation turns the whole thing on in one call, which is
    # what an embedder does when it wants the model to have the tools.
    assert conversation.get_brigade() is None
    installed = conversation.enable_background_agents(4)
    assert conversation.get_brigade() is installed

    print("PASS: all GI binding smoke checks succeeded")
    print(f"  PyGObject {gi.__version__}, AiGlib 1.0 loaded from"
          f" {AiGlib.__path__ if hasattr(AiGlib, '__path__') else '(typelib)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
