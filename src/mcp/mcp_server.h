#ifndef LOOPY_MCP_SERVER_H
#define LOOPY_MCP_SERVER_H

/*
 * Model Context Protocol server, spoken over stdio as newline-delimited
 * JSON-RPC 2.0.  Selected with --mcp.
 *
 * The emulator must already be initialised and the cartridge loaded before this
 * is called: the session is persistent and stateful by design, so a client can
 * run frames, inspect, snapshot, run further, and compare - which is what makes
 * bisecting a timing bug possible at all.  Returns a process exit code once
 * stdin reaches EOF.
 *
 * Because stdout is the transport, nothing else may write to it while the
 * server is running; diagnostics go to stderr.
 */
int loopy_mcp_serve(void);

/*
 * Takes ownership of the real stdout and redirects the C stdout stream to
 * stderr.  Must be called before the emulator is initialised, and before any
 * other code has had a chance to print.
 *
 * stdout is the MCP transport, so a single stray printf anywhere in the core
 * (the loader's "Successfully found SRAM" banner, for one) lands in the middle
 * of the JSON-RPC stream and the client fails to parse the first message.
 * Rather than police every call site forever, the transport is moved somewhere
 * ordinary printf cannot reach it.  Returns 0 on success.
 */
int loopy_mcp_claim_stdout(void);

#endif
