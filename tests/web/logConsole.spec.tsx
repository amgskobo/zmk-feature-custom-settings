import { render, screen, waitFor } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../../web/src/App";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));

afterEach(() => jest.restoreAllMocks());

it("keeps the original error in the UI while logging only safe list diagnostics", async () => {
  const errors = jest.spyOn(console, "error").mockImplementation(() => {});
  const info = jest.spyOn(console, "info").mockImplementation(() => {});
  const debug = jest.spyOn(console, "debug").mockImplementation(() => {});
  (call_rpc as jest.Mock).mockRejectedValue(
    new TypeError("private-setting-name"),
  );
  const app = createConnectedMockZMKApp({ subsystems: [SUBSYSTEM_IDENTIFIER] });
  render(
    <ZMKAppProvider value={app}>
      <RPCTestSection />
    </ZMKAppProvider>,
  );
  await screen.findByText(/private-setting-name/);
  await waitFor(() => expect(errors).toHaveBeenCalled());
  expect(errors).toHaveBeenCalledWith("Initial list failed:", "TypeError");
  expect(debug).not.toHaveBeenCalled();
  expect(
    JSON.stringify([...errors.mock.calls, ...info.mock.calls]),
  ).not.toContain("private-setting-name");
  // The fixed completion reason is part of the label: the console guard keeps
  // only the first string argument and redacts strings inside objects.
  expect(info).toHaveBeenCalledWith(
    "[custom-settings] list complete: request-failed",
    expect.objectContaining({
      collected: 0,
      decodeFailures: 0,
      elapsedMs: expect.any(Number),
    }),
  );
});
