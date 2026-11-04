import { StrictMode } from "react";
import { act, fireEvent, render, screen, within } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../../web/src/App";
import {
  Notification,
  Request,
  Response,
  Setting,
  SettingNotificationKind,
} from "../../web/src/proto/cormoran/zmk/custom_settings/custom_settings";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));
function status(message = "OK") {
  return {
    custom: {
      call: {
        payload: Response.encode(
          Response.create({ status: { affectedCount: 0, message } }),
        ).finish(),
      },
    },
  };
}
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
function app(subsystems = [SUBSYSTEM_IDENTIFIER, "fixture"]) {
  return createConnectedMockZMKApp({
    subsystems,
    deviceName: "Fixture Device",
  });
}
function panel(value: ReturnType<typeof app>, strict = false) {
  const element = (
    <ZMKAppProvider value={value}>
      <RPCTestSection />
    </ZMKAppProvider>
  );
  return strict ? <StrictMode>{element}</StrictMode> : element;
}
async function flush() {
  await act(async () => {
    for (let n = 0; n < 12; n++) {
      await Promise.resolve();
    }
  });
}
async function loaded() {
  await flush();
  await act(async () => {
    jest.advanceTimersByTime(750);
  });
  await flush();
}
function requests() {
  return (call_rpc as jest.Mock).mock.calls.map((c) =>
    Request.decode(c[1].custom.call.payload),
  );
}
function emit(value: ReturnType<typeof app>, setting: Setting) {
  const calls = (value.onNotification as jest.Mock).mock.calls;
  calls[calls.length - 1][0].callback({
    subsystemIndex: 0,
    payload: Notification.encode(
      Notification.create({
        setting: {
          kind: SettingNotificationKind.SETTING_NOTIFICATION_KIND_LIST_ITEM,
          setting,
        },
      }),
    ).finish(),
  });
}
describe("Custom Settings session ownership", () => {
  beforeEach(() => {
    jest.useFakeTimers();
    jest.clearAllMocks();
    (call_rpc as jest.Mock).mockResolvedValue(status());
  });
  afterEach(() => {
    jest.useRealTimers();
  });
  it("retires a delayed read when another setting is selected", async () => {
    const current = app();
    render(panel(current));
    await flush();
    const alpha = Setting.create({
      customSubsystemIndex: 1,
      key: "fixture/alpha",
      source: 0,
      value: { int32Value: 11 },
    });
    const beta = Setting.create({
      customSubsystemIndex: 1,
      key: "fixture/beta",
      source: 0,
      value: { int32Value: 22 },
    });
    emit(current, alpha);
    emit(current, beta);
    await loaded();
    fireEvent.click(screen.getByRole("button", { name: alpha.key }));
    const pending = deferred<ReturnType<typeof status>>();
    (call_rpc as jest.Mock).mockReturnValueOnce(pending.promise);
    fireEvent.click(screen.getByRole("button", { name: "Read" }));
    await flush();
    fireEvent.click(screen.getByRole("button", { name: beta.key }));
    await act(async () => {
      pending.resolve({
        custom: {
          call: {
            payload: Response.encode(
              Response.create({
                getSetting: {
                  setting: { ...alpha, value: { int32Value: 99 } },
                },
              }),
            ).finish(),
          },
        },
      });
    });
    await flush();
    expect(
      screen.getByRole("button", { name: beta.key }).closest("tr"),
    ).toHaveClass("selected-setting-row");
    expect(
      screen.getByRole("button", { name: alpha.key }).closest("tr"),
    ).not.toHaveClass("selected-setting-row");
    const section = screen
      .getByRole("heading", { name: "Update Value" })
      .closest("section")!;
    expect(within(section).getByLabelText(/^Value$/i)).toHaveValue("22");
    fireEvent.click(within(section).getByRole("button", { name: "Write" }));
    await flush();
    const write = requests()
      .filter((request) => request.writeSetting)
      .at(-1)!.writeSetting!;
    expect(write.setting).toMatchObject({ key: beta.key, source: 0 });
    expect(write.value?.int32Value).toBe(22);
    // A current read still updates the selected editor normally.
    (call_rpc as jest.Mock).mockResolvedValueOnce({
      custom: {
        call: {
          payload: Response.encode(
            Response.create({
              getSetting: { setting: { ...beta, value: { int32Value: 77 } } },
            }),
          ).finish(),
        },
      },
    });
    fireEvent.click(screen.getByRole("button", { name: "Read" }));
    await flush();
    expect(within(section).getByLabelText(/^Value$/i)).toHaveValue("77");
  });
  it.each(["connection", "index", "catalog"])(
    "retires old create replies and editor state on %s replacement",
    async (change) => {
      const old = app();
      const view = render(panel(old));
      await loaded();
      const pending = deferred<ReturnType<typeof status>>();
      (call_rpc as jest.Mock).mockReturnValueOnce(pending.promise);
      fireEvent.change(
        screen.getByLabelText("Key", { selector: "#create-key-input" }),
        { target: { value: "fixture/old" } },
      );
      fireEvent.click(screen.getByRole("button", { name: "Create" }));
      await flush();
      expect(requests().filter((r) => r.createSetting)).toHaveLength(1);
      const next = app(
        change === "index"
          ? ["fixture", SUBSYSTEM_IDENTIFIER]
          : change === "catalog"
            ? [SUBSYSTEM_IDENTIFIER, "replacement"]
            : undefined,
      );
      if (change !== "connection") {
        next.state.connection = old.state.connection;
      }
      view.rerender(panel(next));
      await loaded();
      await act(async () => {
        pending.resolve(status("retired-create-result"));
      });
      await flush();
      expect(
        screen.getByLabelText("Key", { selector: "#create-key-input" }),
      ).toHaveValue("");
      expect(
        screen.queryByText(/retired-create-result/),
      ).not.toBeInTheDocument();
      expect(screen.getByRole("button", { name: "Export JSON" })).toBeEnabled();
    },
  );
  it("keeps current-session forms across an unrelated provider update", async () => {
    const current = app();
    const view = render(panel(current));
    await loaded();
    fireEvent.change(
      screen.getByLabelText("Key", { selector: "#create-key-input" }),
      { target: { value: "fixture/current" } },
    );
    view.rerender(panel({ ...current }));
    await loaded();
    expect(
      screen.getByLabelText("Key", { selector: "#create-key-input" }),
    ).toHaveValue("fixture/current");
  });
  it("immediately retires a pending list subscription and timers on unmount", async () => {
    (call_rpc as jest.Mock).mockReturnValue(new Promise(() => {}));
    const current = app();
    const subscribe = (
      current.onNotification as jest.Mock
    ).getMockImplementation()!;
    const unsubscribe = jest.fn();
    (current.onNotification as jest.Mock).mockImplementation((options) => {
      const cleanup = subscribe(options);
      return () => {
        unsubscribe();
        cleanup();
      };
    });
    const view = render(panel(current));
    await flush();
    view.unmount();
    await flush();
    expect(unsubscribe).toHaveBeenCalledTimes(1);
    jest.runAllTicks();
    expect(jest.getTimerCount()).toBe(0);
  });
  it("stops import after the in-flight write when the connection changes", async () => {
    const old = app();
    const view = render(panel(old));
    await loaded();
    const pending = deferred<ReturnType<typeof status>>();
    (call_rpc as jest.Mock).mockReturnValueOnce(pending.promise);
    fireEvent.change(screen.getByLabelText(/Settings JSON/i), {
      target: {
        value: JSON.stringify({
          format: "zmk-custom-settings",
          version: 1,
          customSubsystems: {
            fixture: {
              first: { type: "int32", source: 0, value: 1 },
              second: { type: "int32", source: 0, value: 2 },
            },
          },
        }),
      },
    });
    fireEvent.click(screen.getByRole("button", { name: "Import JSON" }));
    await flush();
    expect(requests().filter((r) => r.writeSetting)).toHaveLength(1);
    view.rerender(panel(app()));
    await loaded();
    await act(async () => {
      pending.resolve(status());
    });
    await flush();
    expect(requests().filter((r) => r.writeSetting)).toHaveLength(1);
    expect(screen.getByLabelText(/Settings JSON/i)).toHaveValue("");
    expect(screen.queryByText(/Imported 2/)).not.toBeInTheDocument();
  });
  it("stops chunk writes after the in-flight chunk on connection replacement", async () => {
    const old = app();
    const view = render(panel(old));
    await flush();
    emit(
      old,
      Setting.create({
        customSubsystemIndex: 1,
        key: "large_bytes",
        source: 0,
        value: { bytesValue: new Uint8Array([1]) },
      }),
    );
    await loaded();
    fireEvent.click(screen.getByRole("button", { name: "large_bytes" }));
    const section = screen
      .getByRole("heading", { name: "Update Value" })
      .closest("section")!;
    fireEvent.change(within(section).getByLabelText(/^Value$/i), {
      target: { value: Array(300).fill("1").join(",") },
    });
    const pending = deferred<ReturnType<typeof status>>();
    (call_rpc as jest.Mock).mockReturnValueOnce(pending.promise);
    fireEvent.click(within(section).getByRole("button", { name: "Write" }));
    await flush();
    expect(requests().filter((r) => r.writeValueChunk)).toHaveLength(1);
    view.rerender(panel(app()));
    await loaded();
    await act(async () => {
      pending.resolve(status());
    });
    await flush();
    expect(requests().filter((r) => r.writeValueChunk)).toHaveLength(1);
    expect(
      screen.queryByRole("heading", { name: "Update Value" }),
    ).not.toBeInTheDocument();
  });
  it("keeps the current StrictMode session usable after cleanup replay", async () => {
    const current = app();
    render(panel(current, true));
    await loaded();
    expect(screen.getByRole("button", { name: "Export JSON" })).toBeEnabled();
    expect(screen.queryByText(/session.*retired/i)).not.toBeInTheDocument();
  });
  it("bounds collection even when list notifications keep arriving", async () => {
    const current = app();
    render(panel(current));
    await flush();
    const setting = Setting.create({
      customSubsystemIndex: 1,
      key: "streamed",
      source: 0,
      value: { int32Value: 1 },
    });
    for (let i = 0; i < 7; i++) {
      await act(async () => {
        jest.advanceTimersByTime(700);
      });
      await act(async () => {
        emit(current, setting);
      });
    }
    await act(async () => {
      jest.advanceTimersByTime(101);
    });
    await flush();
    expect(
      screen.getByText(/List notifications timed out/),
    ).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Export JSON" })).toBeEnabled();
    jest.runAllTicks();
    expect(jest.getTimerCount()).toBe(0);
  });
});
