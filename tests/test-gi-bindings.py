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
import sys


def main():
    try:
        import gi
    except ImportError:
        print("SKIP: python3-gobject not installed", file=sys.stderr)
        return 0

    gi.require_version("AiGlib", "1.0")
    from gi.repository import AiGlib  # noqa: E402

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

    # ----- Boxed type roundtrip -----
    usage = AiGlib.Usage.new(100, 50)
    assert usage.get_input_tokens() == 100
    assert usage.get_output_tokens() == 50
    assert usage.get_total_tokens() == 150

    print("PASS: all GI binding smoke checks succeeded")
    print(f"  PyGObject {gi.__version__}, AiGlib 1.0 loaded from"
          f" {AiGlib.__path__ if hasattr(AiGlib, '__path__') else '(typelib)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
