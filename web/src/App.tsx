import { errorKind } from "./logPrivacy";
import { Fragment, useContext, useEffect, useRef, useState } from "react";
import "./App.css";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import { ZMKConnection, ZMKAppContext } from "@cormoran/zmk-studio-react-hook";
import {
  Notification as SettingsNotification,
  Request,
  Response,
  Setting,
  SettingConfidentiality,
  SettingConstraint,
  SettingMeta,
  SettingPermission,
  SettingScope,
  SettingScalarValue,
  SettingValue,
  SettingNotificationKind,
  SettingWriteMode,
} from "./proto/cormoran/zmk/custom_settings/custom_settings";
import {
  createSettingsExportDocument,
  countExportedSettings,
  exportedSettingValueToProto,
  parseSettingsExportJson,
} from "./settingsJson";
import {
  constraintKind,
  getActiveConstraint,
  isOptionsPossiblyTruncated,
  optionEntries,
  scalarValueToEditorString,
  scalarValueToNumber,
  validateConstraintValue,
} from "./constraints";
import { writeValueChunked, SINGLE_FRAME_VALUE_MAX } from "./chunkedValue";

export const SUBSYSTEM_IDENTIFIER = "cormoran_custom_settings";
const LIST_NOTIFICATION_TIMEOUT_MS = 750;
const LIST_REQUEST_TIMEOUT_MS = 5000;
const SOURCE_LOCAL = 0;
const SOURCE_ALL = 0xffffffff;

type SettingGroup = {
  key: string;
  setting: Setting;
  settings: Setting[];
};

type SubsystemGroup = {
  customSubsystemIndex: number;
  groups: SettingGroup[];
};

enum EditorValueType {
  Int32 = "int32",
  Bool = "bool",
  String = "string",
  Bytes = "bytes",
  Behavior = "behavior",
}

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>ZMK Custom Settings</h1>
        <p>Device settings console</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>{error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <RPCTestSection />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>Settings module</strong>
        </p>
      </footer>
    </div>
  );
}

const connectionKeys = new WeakMap<object, number>();
let nextConnectionKey = 0;
function connectionKey(connection: object | null | undefined): number {
  if (!connection) return 0;
  let key = connectionKeys.get(connection);
  if (key === undefined) {
    key = ++nextConnectionKey;
    connectionKeys.set(connection, key);
  }
  return key;
}

export function RPCTestSection() {
  const app = useContext(ZMKAppContext);
  const index = app?.findSubsystem(SUBSYSTEM_IDENTIFIER)?.index;
  const catalog = JSON.stringify(
    app?.state.customSubsystems?.subsystems.map((s) => s.identifier) ?? []
  );
  const key = `${connectionKey(app?.state.connection)}:${index}:${catalog}`;
  return <SettingsSession key={key} />;
}

function SettingsSession() {
  const lifetime = useRef({ active: true, cancel: new Set<() => void>() });
  useEffect(() => {
    const session = lifetime.current;
    session.active = true;
    return () => {
      session.active = false;
      for (const cancel of [...session.cancel]) {
        cancel();
      }
      session.cancel.clear();
    };
  }, []);
  const zmkApp = useContext(ZMKAppContext);
  const [filterCustomSubsystemId, setFilterCustomSubsystemId] = useState("");
  const [filterKey, setFilterKey] = useState("");
  const [filterKeyPrefix, setFilterKeyPrefix] = useState("");
  const [editorCustomSubsystemId, setEditorCustomSubsystemId] = useState("");
  const [editorSettingKey, setEditorSettingKey] = useState("");
  const [createCustomSubsystemId, setCreateCustomSubsystemId] = useState("");
  const [createSettingKey, setCreateSettingKey] = useState("");
  const [createValueType, setCreateValueType] = useState<EditorValueType>(
    EditorValueType.Int32
  );
  const [createValue, setCreateValue] = useState("0");
  const [createWriteMode, setCreateWriteMode] = useState<SettingWriteMode>(
    SettingWriteMode.SETTING_WRITE_MODE_MEMORY
  );
  const [valueType, setValueType] = useState<EditorValueType>(
    EditorValueType.Int32
  );
  const [value, setValue] = useState("10");
  const [isArray, setIsArray] = useState(false);
  const [arrayIndex, setArrayIndex] = useState(0);
  const [arraySize, setArraySize] = useState(1);
  const [editorSource, setEditorSource] = useState(SOURCE_LOCAL);
  const [requireMeta, setRequireMeta] = useState(true);
  const [requireDefault, setRequireDefault] = useState(false);
  const [writeMode, setWriteMode] = useState<SettingWriteMode>(
    SettingWriteMode.SETTING_WRITE_MODE_MEMORY
  );
  const [setting, setSetting] = useState<Setting | null>(null);
  const [listedSettings, setListedSettings] = useState<Setting[]>([]);
  const [response, setResponse] = useState<string | null>(null);
  const [jsonText, setJsonText] = useState("");
  const [isLoading, setIsLoading] = useState(false);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER) ?? undefined;
  const trimmedFilterCustomSubsystemId = filterCustomSubsystemId.trim();
  const trimmedEditorCustomSubsystemId = editorCustomSubsystemId.trim();
  const filterSubsystem = trimmedFilterCustomSubsystemId
    ? zmkApp?.findSubsystem(trimmedFilterCustomSubsystemId)
    : undefined;
  const editorSubsystem = trimmedEditorCustomSubsystemId
    ? zmkApp?.findSubsystem(trimmedEditorCustomSubsystemId)
    : undefined;

  const settingRef = {
    customSubsystemIndex: editorSubsystem?.index,
    key: editorSettingKey,
    source: editorSource,
    arrayIndex: isArray ? arrayIndex : undefined,
  };

  const settingScope = {
    customSubsystemIndex: filterSubsystem?.index,
    key: optionalString(filterKey),
    keyPrefix: optionalString(filterKeyPrefix),
    source: SOURCE_ALL,
  };

  const listScope = {
    ...settingScope,
    source: SOURCE_ALL,
  };

  const parseScalarValue = (): SettingScalarValue =>
    parseScalarValueOf(valueType, value);

  const parseValue = (): SettingValue => {
    const scalarValue = parseScalarValue();
    if (isArray) {
      return {
        arrayValue: {
          index: arrayIndex,
          size: arraySize,
          value: scalarValue,
        },
      };
    }

    return scalarValue;
  };

  const callCustomRequest = async (request: Request): Promise<Response> => {
    const session = lifetime.current;
    if (!session.active) {
      throw new Error("Settings session retired");
    }
    const connection = zmkApp?.state.connection;
    if (!connection || !subsystem) {
      throw new Error("Custom settings subsystem is not available");
    }

    const payload = Request.encode(request).finish();
    let retire = () => {};
    const retired = new Promise<never>((_, reject) => {
      retire = () => reject(new Error("Settings session retired"));
      session.cancel.add(retire);
    });
    try {
      // Own the deadline so retirement also clears its timer. The SDK
      // service wrapper leaves its timeout pending after a completed call.
      const response = await withTimeout(
        Promise.race([
          call_rpc(connection, {
            custom: { call: { subsystemIndex: subsystem.index, payload } },
          }),
          retired,
        ]),
        LIST_REQUEST_TIMEOUT_MS,
        "RPC request timed out"
      );
      const responsePayload = response.custom?.call?.payload;
      if (!session.active) {
        throw new Error("Settings session retired");
      }
      if (!responsePayload) {
        throw new Error("Empty response");
      }
      return Response.decode(responsePayload);
    } finally {
      session.cancel.delete(retire);
    }
  };

  const subsystemIdentifierForIndex = (index: number) =>
    zmkApp?.state.customSubsystems?.subsystems[index]?.identifier;

  const subsystemIndexForIdentifier = (identifier: string) =>
    zmkApp?.findSubsystem(identifier)?.index;

  const sendRequest = async (request: Request) => {
    setIsLoading(true);
    setResponse(null);

    try {
      const resp = await callCustomRequest(request);
      if (resp.getSetting?.setting) {
        setSetting(resp.getSetting.setting);
        setResponse(formatSetting(resp.getSetting.setting));
      } else if (resp.status) {
        setResponse(
          `${resp.status.message || "OK"} (${resp.status.affectedCount})`
        );
      } else if (resp.error) {
        setResponse(`Error: ${resp.error.message}`);
      }
    } catch (error) {
      console.error("RPC call failed:", errorKind(error));
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const collectListSettings = async (
    requestScope: SettingScope = {
      source: SOURCE_ALL,
    },
    includeMeta = false,
    includeDefault = false
  ): Promise<Setting[]> => {
    if (!zmkApp || !subsystem) return [];

    const startedAt = performance.now();
    let decodeFailures = 0;
    let completionReason = "request-failed";
    console.info("[custom-settings] list start", {
      allSources: scopeTargetsAll(requestScope),
      requireMeta: includeMeta,
      requireDefault: includeDefault,
    });
    const collected: Setting[] = [];
    const waitForQuiet = scopeTargetsAll(requestScope);
    let expectedCount: number | undefined;
    let quietTimeout: ReturnType<typeof setTimeout> | undefined;
    let collectionTimeout: ReturnType<typeof setTimeout> | undefined;
    let collectionFailure: Error | undefined;
    let resolveList: () => void = () => {};
    let isComplete = false;

    const listComplete = new Promise<void>((resolve) => {
      resolveList = resolve;
    });

    const completeList = (reason: string) => {
      if (isComplete) {
        return;
      }
      isComplete = true;
      if (quietTimeout) {
        clearTimeout(quietTimeout);
        quietTimeout = undefined;
      }
      if (collectionTimeout) {
        clearTimeout(collectionTimeout);
        collectionTimeout = undefined;
      }
      completionReason = reason;
      resolveList();
    };

    const scheduleQuietResolve = () => {
      if (quietTimeout) {
        clearTimeout(quietTimeout);
      }
      quietTimeout = setTimeout(() => {
        completeList("quiet-timeout");
      }, LIST_NOTIFICATION_TIMEOUT_MS);
    };

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (customNotification) => {
        if (!lifetime.current.active || isComplete) return;
        let notification;
        try {
          notification = SettingsNotification.decode(
            customNotification.payload
          );
        } catch {
          decodeFailures++;
          return;
        }
        if (
          notification.setting?.kind ===
            SettingNotificationKind.SETTING_NOTIFICATION_KIND_LIST_ITEM &&
          notification.setting.setting
        ) {
          collected.push(notification.setting.setting);
          if (
            expectedCount !== undefined &&
            !waitForQuiet &&
            collected.length >= expectedCount
          ) {
            completeList("expected-count");
          } else if (waitForQuiet) {
            scheduleQuietResolve();
          }
        }
      },
    });

    let subscribed = true;
    const stopSubscription = () => {
      if (subscribed) {
        subscribed = false;
        unsubscribe();
      }
    };
    const retireList = () => {
      stopSubscription();
      completeList("session-retired");
    };
    lifetime.current.cancel.add(retireList);

    try {
      const resp = await withTimeout(
        callCustomRequest(
          Request.create({
            listSettings: {
              scope: requestScope,
              requireMeta: includeMeta,
              requireDefault: includeDefault,
            },
          })
        ),
        LIST_REQUEST_TIMEOUT_MS,
        "List request timed out"
      );
      if (resp.error) {
        throw new Error(resp.error.message || "List failed");
      }
      expectedCount = resp.status?.affectedCount;
      collectionTimeout = setTimeout(() => {
        collectionFailure = new Error("List notifications timed out");
        completeList("notification-timeout");
      }, LIST_REQUEST_TIMEOUT_MS);

      if (!waitForQuiet && expectedCount !== undefined) {
        if (collected.length >= expectedCount) {
          completeList("expected-count");
        }
      } else {
        scheduleQuietResolve();
      }
      await listComplete;
      if (!lifetime.current.active) throw new Error("Settings session retired");
      if (collectionFailure) throw collectionFailure;
    } finally {
      lifetime.current.cancel.delete(retireList);
      stopSubscription();
      if (collectionTimeout) clearTimeout(collectionTimeout);
      if (quietTimeout) {
        clearTimeout(quietTimeout);
      }
      // The reason is one of this file's fixed literals; the console guard
      // keeps only the first string argument.
      console.info(`[custom-settings] list complete: ${completionReason}`, {
        collected: collected.length,
        expectedCount,
        decodeFailures,
        elapsedMs: Math.max(0, Math.round(performance.now() - startedAt)),
      });
    }

    return collected;
  };

  const exportJson = async () => {
    setIsLoading(true);
    setResponse(null);

    try {
      const settings = await collectListSettings();
      const exported = createSettingsExportDocument(
        settings,
        subsystemIdentifierForIndex
      );
      setJsonText(JSON.stringify(exported, null, 2));
      setResponse(`Exported ${countExportedSettings(exported)} setting values`);
    } catch (error) {
      console.error("Export failed:", errorKind(error));
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const importJson = async () => {
    setIsLoading(true);
    setResponse(null);

    try {
      const settings = parseSettingsExportJson(jsonText);
      for (const importedSetting of settings) {
        const customSubsystemIndex = subsystemIndexForIdentifier(
          importedSetting.customSubsystemId
        );
        if (customSubsystemIndex === undefined) {
          throw new Error(
            `Custom subsystem not found: ${importedSetting.customSubsystemId}`
          );
        }

        const resp = await callCustomRequest(
          Request.create({
            writeSetting: {
              setting: {
                customSubsystemIndex,
                key: importedSetting.key,
                source: importedSetting.source,
                arrayIndex: importedSetting.arrayIndex,
              },
              value: exportedSettingValueToProto(importedSetting),
              mode: writeMode,
            },
          })
        );
        if (resp.error) {
          throw new Error(
            `${importedSetting.customSubsystemId}/${importedSetting.key}: ${resp.error.message}`
          );
        }
      }

      setResponse(`Imported ${settings.length} setting values`);
    } catch (error) {
      console.error("Import failed:", errorKind(error));
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const getSetting = async () => {
    setIsLoading(true);
    setResponse(null);

    try {
      const resp = await callCustomRequest(
        Request.create({
          getSetting: { setting: settingRef, requireMeta, requireDefault },
        })
      );
      if (resp.error) {
        setResponse(`Error: ${resp.error.message}`);
        return;
      }

      const fetched = resp.getSetting?.setting;
      if (!fetched) {
        setResponse("No setting returned");
        return;
      }

      setSetting(fetched);
      // GetSetting streams a value of any size directly, so formatSetting has
      // everything it needs - a missing `value` here means the setting is
      // device-private or secure while locked.
      setResponse(formatSetting(fetched));
    } catch (error) {
      console.error("Get setting failed:", errorKind(error));
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const listSettings = async () => {
    setIsLoading(true);
    setResponse(null);

    try {
      if (trimmedFilterCustomSubsystemId && !filterSubsystem) {
        throw new Error(
          `Custom subsystem not found: ${trimmedFilterCustomSubsystemId}`
        );
      }

      const settings = await collectListSettings(
        listScope,
        requireMeta,
        requireDefault
      );
      setListedSettings(settings);
      setResponse(`Listed ${settings.length} settings`);
    } catch (error) {
      console.error("List failed:", errorKind(error));
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const writeSetting = async () => {
    // A BYTES/STRING value larger than one frame cannot ride the single-frame
    // SettingValue field, so it is written over the chunked WriteValueChunk
    // RPC. Small values and arrays keep the plain write path.
    if (
      !isArray &&
      (valueType === EditorValueType.Bytes ||
        valueType === EditorValueType.String)
    ) {
      const bytes =
        valueType === EditorValueType.Bytes
          ? parseBytesValue(value)
          : new TextEncoder().encode(value);
      if (bytes.length > SINGLE_FRAME_VALUE_MAX) {
        setIsLoading(true);
        setResponse(null);
        try {
          await writeValueChunked(
            callCustomRequest,
            settingRef,
            bytes,
            writeMode
          );
          setResponse(`Wrote ${bytes.length} bytes via chunked transfer`);
        } catch (error) {
          console.error("Chunked write failed:", errorKind(error));
          setResponse(
            `Failed: ${
              error instanceof Error ? error.message : "Unknown error"
            }`
          );
        } finally {
          setIsLoading(false);
        }
        return;
      }
    }

    return sendRequest(
      Request.create({
        writeSetting: {
          setting: settingRef,
          value: parseValue(),
          mode: writeMode,
        },
      })
    );
  };

  const pushBackArray = () =>
    sendRequest(
      Request.create({
        pushBackArray: {
          setting: settingRef,
          value: parseScalarValue(),
          mode: writeMode,
        },
      })
    );

  const popBackArray = () =>
    sendRequest(
      Request.create({
        popBackArray: {
          setting: settingRef,
          mode: writeMode,
        },
      })
    );

  const trimmedCreateCustomSubsystemId = createCustomSubsystemId.trim();
  const createSubsystem = trimmedCreateCustomSubsystemId
    ? zmkApp?.findSubsystem(trimmedCreateCustomSubsystemId)
    : undefined;

  const createSetting = async () => {
    setIsLoading(true);
    setResponse(null);

    try {
      if (trimmedCreateCustomSubsystemId && !createSubsystem) {
        throw new Error(
          `Custom subsystem not found: ${trimmedCreateCustomSubsystemId}`
        );
      }

      const resp = await callCustomRequest(
        Request.create({
          createSetting: {
            setting: {
              customSubsystemIndex: createSubsystem?.index,
              key: createSettingKey,
            },
            value: parseScalarValueOf(createValueType, createValue),
            mode: createWriteMode,
          },
        })
      );
      if (resp.error) {
        throw new Error(resp.error.message || "Create failed");
      }
      setResponse(
        `${resp.status?.message || "Created"} (${resp.status?.affectedCount ?? 1})`
      );
    } catch (error) {
      console.error("Create setting failed:", errorKind(error));
      setResponse(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setIsLoading(false);
    }
  };

  const deleteSetting = () =>
    sendRequest(
      Request.create({
        deleteSetting: {
          setting: settingRef,
        },
      })
    );

  const saveSettings = () =>
    sendRequest(Request.create({ saveSettings: { scope: settingScope } }));

  const discardSettings = () =>
    sendRequest(Request.create({ discardSettings: { scope: settingScope } }));

  const resetSettings = () =>
    sendRequest(Request.create({ resetSettings: { scope: settingScope } }));

  const applySettingToEditor = (
    nextSetting: Setting,
    nextSource = nextSetting.source
  ) => {
    setSetting(nextSetting);
    setEditorSource(nextSource);
    setEditorCustomSubsystemId(
      subsystemIdentifierForIndex(nextSetting.customSubsystemIndex) ??
        `${nextSetting.customSubsystemIndex}`
    );
    setEditorSettingKey(nextSetting.key);

    const arrayValue = nextSetting.value?.arrayValue;
    setIsArray(arrayValue !== undefined);
    if (arrayValue) {
      setArrayIndex(arrayValue.index);
      setArraySize(arrayValue.size);
      applyScalarValueToEditor(arrayValue.value ?? {});
    } else if (nextSetting.value) {
      applyScalarValueToEditor(nextSetting.value);
    } else {
      setValueType(EditorValueType.Int32);
      setValue("");
    }
  };

  const selectSettingForEdit = (nextSetting: Setting) => {
    if (setting && sameSettingIdentity(setting, nextSetting)) {
      setSetting(null);
      return;
    }

    applySettingToEditor(nextSetting);
  };

  const handleEditorSourceChange = (nextSource: number) => {
    if (!setting) {
      setEditorSource(nextSource);
      return;
    }

    const matchingSetting = listedSettings.find(
      (listedSetting) =>
        sameSettingIdentity(setting, listedSetting) &&
        listedSetting.source === nextSource
    );
    if (matchingSetting) {
      applySettingToEditor(matchingSetting, nextSource);
    } else {
      setEditorSource(nextSource);
    }
  };

  const applyScalarValueToEditor = (nextValue: SettingScalarValue) => {
    if (nextValue.int32Value !== undefined) {
      setValueType(EditorValueType.Int32);
      setValue(`${nextValue.int32Value}`);
    } else if (nextValue.boolValue !== undefined) {
      setValueType(EditorValueType.Bool);
      setValue(nextValue.boolValue ? "true" : "false");
    } else if (nextValue.stringValue !== undefined) {
      setValueType(EditorValueType.String);
      setValue(nextValue.stringValue);
    } else if (nextValue.bytesValue !== undefined) {
      setValueType(EditorValueType.Bytes);
      setValue(formatBytesValue(nextValue.bytesValue));
    } else if (nextValue.behaviorValue !== undefined) {
      setValueType(EditorValueType.Behavior);
      setValue(formatBehaviorValue(nextValue.behaviorValue));
    }
  };

  useEffect(() => {
    if (!subsystem) {
      return;
    }

    let cancelled = false;

    const loadAllSettings = async () => {
      setIsLoading(true);
      setResponse(null);

      try {
        const settings = await collectListSettings(
          { source: SOURCE_ALL },
          requireMeta,
          requireDefault
        );
        if (cancelled) {
          return;
        }
        setListedSettings(settings);
        setResponse(`Listed ${settings.length} settings`);
      } catch (error) {
        if (!cancelled) {
          console.error("Initial list failed:", errorKind(error));
          setResponse(
            `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
          );
        }
      } finally {
        if (!cancelled) {
          setIsLoading(false);
        }
      }
    };

    void loadAllSettings();

    return () => {
      cancelled = true;
    };
    // Run once when the connected custom settings subsystem becomes available.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the custom settings module.
          </p>
        </div>
      </section>
    );
  }

  const selectedSettingIsArray = setting?.value?.arrayValue !== undefined;
  const selectedSettingDisplayName = setting
    ? settingDisplayName(setting, subsystemIdentifierForIndex)
    : "";
  const selectedSettingValueType = setting
    ? settingValueTypeLabel(setting.value)
    : undefined;
  const selectedSettingValueHidden =
    setting !== null && setting.value === undefined;
  const selectedSourceOptions = setting
    ? sourceOptionsForSetting(setting, listedSettings)
    : [];
  const subsystemGroups = groupBySubsystem(groupSettings(listedSettings));
  const activeConstraint = getActiveConstraint(setting?.meta);
  const constraintError = validateConstraintValue(activeConstraint, value);
  const renderValueEditor = () => {
    const options = activeConstraint?.options;
    if (options) {
      const entries = optionEntries(options);
      const hasCurrent = entries.some((entry) => entry.value === value);
      return (
        <>
          <label htmlFor="value-input">Value</label>
          <div className="constraint-control">
            <select
              id="value-input"
              value={value}
              onChange={(e) => setValue(e.target.value)}
            >
              {!hasCurrent && (
                <option value={value}>
                  {value === "" ? "-- select --" : `(current: ${value})`}
                </option>
              )}
              {entries.map((entry) => (
                <option key={entry.value} value={entry.value}>
                  {entry.label}
                </option>
              ))}
            </select>
            {isOptionsPossiblyTruncated(options) && (
              <p className="hint-text">
                Option list may be truncated to 8 entries by RPC.
              </p>
            )}
            {constraintError && (
              <p className="constraint-error" role="alert">
                {constraintError}
              </p>
            )}
          </div>
        </>
      );
    }

    const range = activeConstraint?.range;
    if (range) {
      const min = scalarValueToNumber(range.min);
      const max = scalarValueToNumber(range.max);
      const sliderValue =
        Number.isNaN(Number(value)) || value.trim() === ""
          ? `${min ?? 0}`
          : value;
      return (
        <>
          <label htmlFor="value-input">Value</label>
          <div className="constraint-control">
            <input
              id="value-input"
              type="number"
              min={min}
              max={max}
              value={value}
              onChange={(e) => setValue(e.target.value)}
            />
            {min !== undefined && max !== undefined && (
              <input
                type="range"
                aria-label="Value slider"
                min={min}
                max={max}
                value={sliderValue}
                onChange={(e) => setValue(e.target.value)}
              />
            )}
            <p className="hint-text">
              Allowed range: {min ?? "?"} – {max ?? "?"}.
            </p>
            {constraintError && (
              <p className="constraint-error" role="alert">
                {constraintError}
              </p>
            )}
          </div>
        </>
      );
    }

    const hidUsage = activeConstraint?.hidUsage;
    if (hidUsage) {
      return (
        <>
          <label htmlFor="value-input">Value</label>
          <div className="constraint-control">
            <input
              id="value-input"
              type="number"
              min={hidUsage.usageMin}
              max={hidUsage.usageMax > 0 ? hidUsage.usageMax : undefined}
              value={value}
              onChange={(e) => setValue(e.target.value)}
            />
            <p className="hint-text">
              HID usage page {hidUsage.usagePage}; usage {hidUsage.usageMin}
              {hidUsage.usageMax > 0 ? ` – ${hidUsage.usageMax}` : "+"}.
            </p>
            {constraintError && (
              <p className="constraint-error" role="alert">
                {constraintError}
              </p>
            )}
          </div>
        </>
      );
    }

    if (activeConstraint?.layerId) {
      return (
        <>
          <label htmlFor="value-input">Value</label>
          <div className="constraint-control">
            <input
              id="value-input"
              type="number"
              min={0}
              value={value}
              onChange={(e) => setValue(e.target.value)}
            />
            <p className="hint-text">
              Layer index. No layer list is available over RPC, so enter the
              layer number directly.
            </p>
            {constraintError && (
              <p className="constraint-error" role="alert">
                {constraintError}
              </p>
            )}
          </div>
        </>
      );
    }

    if (activeConstraint?.behaviorId) {
      return (
        <>
          <label htmlFor="value-input">Value</label>
          <div className="constraint-control">
            <input
              id="value-input"
              value={value}
              onChange={(e) => setValue(e.target.value)}
              placeholder="behaviorId,param1,param2"
            />
            <p className="hint-text">
              Behavior binding. No behavior list is available over RPC, so enter
              behaviorId,param1,param2 directly.
            </p>
            {constraintError && (
              <p className="constraint-error" role="alert">
                {constraintError}
              </p>
            )}
          </div>
        </>
      );
    }

    return (
      <>
        <label htmlFor="value-input">Value</label>
        <input
          id="value-input"
          value={value}
          onChange={(e) => setValue(e.target.value)}
          placeholder={
            valueType === EditorValueType.Behavior
              ? "behaviorId,param1,param2"
              : undefined
          }
        />
      </>
    );
  };
  const updateValuePanel = setting ? (
    <section className="settings-panel settings-editor-panel">
      <h3>Update Value</h3>
      <div className="form-grid">
        <span className="form-label">Setting</span>
        <span className="form-value">{selectedSettingDisplayName}</span>

        <label htmlFor="source-input">Source</label>
        <select
          id="source-input"
          value={editorSource}
          onChange={(e) => handleEditorSourceChange(Number(e.target.value))}
        >
          {selectedSourceOptions.map((source) => (
            <option key={source} value={source}>
              {formatSource(source)}
            </option>
          ))}
        </select>

        {selectedSettingValueType ? (
          <>
            <span className="form-label">Type</span>
            <span className="form-value">{selectedSettingValueType}</span>
          </>
        ) : (
          <>
            <label htmlFor="type-input">Type</label>
            <select
              id="type-input"
              value={valueType}
              onChange={(e) => setValueType(e.target.value as EditorValueType)}
            >
              <option value={EditorValueType.Int32}>int32</option>
              <option value={EditorValueType.Bool}>bool</option>
              <option value={EditorValueType.String}>string</option>
              <option value={EditorValueType.Bytes}>bytes</option>
              <option value={EditorValueType.Behavior}>behavior</option>
            </select>
          </>
        )}

        {selectedSettingIsArray && (
          <>
            <span className="form-label">Array Index</span>
            <span className="form-value">{arrayIndex}</span>

            <span className="form-label">Array Size</span>
            <span className="form-value">{arraySize}</span>
          </>
        )}

        {renderValueEditor()}

        <label htmlFor="mode-input">Write Mode</label>
        <select
          id="mode-input"
          value={writeMode}
          onChange={(e) => setWriteMode(Number(e.target.value))}
        >
          <option value={SettingWriteMode.SETTING_WRITE_MODE_MEMORY}>
            memory
          </option>
          <option value={SettingWriteMode.SETTING_WRITE_MODE_PERSIST}>
            persist
          </option>
        </select>
      </div>

      <div className="toolbar">
        <button className="btn" disabled={isLoading} onClick={getSetting}>
          Read
        </button>
        <button
          className="btn btn-primary"
          disabled={
            isLoading || selectedSettingValueHidden || constraintError !== null
          }
          onClick={writeSetting}
        >
          Write
        </button>
        {selectedSettingIsArray && (
          <>
            <button
              className="btn"
              disabled={
                isLoading ||
                selectedSettingValueHidden ||
                constraintError !== null
              }
              onClick={pushBackArray}
            >
              Push Back
            </button>
            <button className="btn" disabled={isLoading} onClick={popBackArray}>
              Pop Back
            </button>
          </>
        )}
        <button
          className="btn btn-danger"
          disabled={isLoading}
          onClick={deleteSetting}
        >
          Delete
        </button>
      </div>
    </section>
  ) : null;

  return (
    <section className="card">
      <h2>Settings</h2>

      <section className="settings-panel">
        <h3>Filter</h3>
        <div className="form-grid">
          <label htmlFor="filter-custom-subsystem-input">
            Filter Subsystem
          </label>
          <input
            id="filter-custom-subsystem-input"
            value={filterCustomSubsystemId}
            onChange={(e) => setFilterCustomSubsystemId(e.target.value)}
          />

          <label htmlFor="filter-key-input">Filter Key</label>
          <input
            id="filter-key-input"
            value={filterKey}
            onChange={(e) => setFilterKey(e.target.value)}
          />

          <label htmlFor="filter-prefix-input">Key Prefix</label>
          <input
            id="filter-prefix-input"
            value={filterKeyPrefix}
            onChange={(e) => setFilterKeyPrefix(e.target.value)}
          />

          <label htmlFor="require-meta-input">Include Metadata</label>
          <input
            id="require-meta-input"
            type="checkbox"
            checked={requireMeta}
            onChange={(e) => setRequireMeta(e.target.checked)}
          />

          <label htmlFor="require-default-input">Include Default</label>
          <input
            id="require-default-input"
            type="checkbox"
            checked={requireDefault}
            onChange={(e) => setRequireDefault(e.target.checked)}
          />
        </div>

        <div className="toolbar">
          <button className="btn" disabled={isLoading} onClick={listSettings}>
            List
          </button>
          <button className="btn" disabled={isLoading} onClick={saveSettings}>
            Save
          </button>
          <button
            className="btn"
            disabled={isLoading}
            onClick={discardSettings}
          >
            Discard
          </button>
          <button
            className="btn btn-danger"
            disabled={isLoading}
            onClick={resetSettings}
          >
            Reset
          </button>
        </div>
      </section>

      <section className="settings-panel">
        <h3>Create Setting</h3>
        <p className="hint-text">
          Creates a new entry in an RPC-creatable keyspace (e.g. a module
          registering `ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE` with prefix
          "macro/"). The key must start with a registered keyspace's prefix.
        </p>
        <div className="form-grid">
          <label htmlFor="create-custom-subsystem-input">Subsystem</label>
          <input
            id="create-custom-subsystem-input"
            value={createCustomSubsystemId}
            onChange={(e) => setCreateCustomSubsystemId(e.target.value)}
          />

          <label htmlFor="create-key-input">Key</label>
          <input
            id="create-key-input"
            value={createSettingKey}
            onChange={(e) => setCreateSettingKey(e.target.value)}
            placeholder="macro/my-macro-1"
          />

          <label htmlFor="create-type-input">Type</label>
          <select
            id="create-type-input"
            value={createValueType}
            onChange={(e) =>
              setCreateValueType(e.target.value as EditorValueType)
            }
          >
            <option value={EditorValueType.Int32}>int32</option>
            <option value={EditorValueType.Bool}>bool</option>
            <option value={EditorValueType.String}>string</option>
            <option value={EditorValueType.Bytes}>bytes</option>
            <option value={EditorValueType.Behavior}>behavior</option>
          </select>

          <label htmlFor="create-value-input">Value</label>
          <input
            id="create-value-input"
            value={createValue}
            onChange={(e) => setCreateValue(e.target.value)}
          />

          <label htmlFor="create-mode-input">Write Mode</label>
          <select
            id="create-mode-input"
            value={createWriteMode}
            onChange={(e) => setCreateWriteMode(Number(e.target.value))}
          >
            <option value={SettingWriteMode.SETTING_WRITE_MODE_MEMORY}>
              memory
            </option>
            <option value={SettingWriteMode.SETTING_WRITE_MODE_PERSIST}>
              persist
            </option>
          </select>
        </div>

        <div className="toolbar">
          <button
            className="btn btn-primary"
            disabled={isLoading || createSettingKey.trim().length === 0}
            onClick={createSetting}
          >
            Create
          </button>
        </div>
      </section>

      <div className="settings-list">
        <div className="settings-list-header">
          <h3>Device Settings</h3>
          <span>{listedSettings.length} loaded</span>
        </div>
        {subsystemGroups.length > 0 ? (
          subsystemGroups.map((subsystemGroup) => (
            <div
              className="settings-subsystem"
              key={subsystemGroup.customSubsystemIndex}
            >
              <h4 className="settings-subsystem-title">
                {subsystemIdentifierForIndex(
                  subsystemGroup.customSubsystemIndex
                ) ?? subsystemGroup.customSubsystemIndex}
              </h4>
              <div className="settings-table-wrap">
                <table className="settings-table">
                  <thead>
                    <tr>
                      <th>Setting</th>
                      <th>Confidentiality</th>
                      <th>Read</th>
                      <th>Write</th>
                      <th>Constraints</th>
                      <th>Source</th>
                      <th>Value</th>
                      <th>Unsaved</th>
                    </tr>
                  </thead>
                  <tbody>
                    {subsystemGroup.groups.map((group) => {
                      const isSelected =
                        setting !== null &&
                        sameSettingIdentity(setting, group.setting);
                      const meta = group.setting.meta;
                      return (
                        <Fragment key={group.key}>
                          {group.settings.map((groupSetting, index) => (
                            <tr
                              className={
                                isSelected ? "selected-setting-row" : ""
                              }
                              key={`${groupSetting.source}:${index}`}
                            >
                              {index === 0 && (
                                <>
                                  <td rowSpan={group.settings.length}>
                                    <button
                                      className="link-button"
                                      type="button"
                                      onClick={() =>
                                        selectSettingForEdit(group.setting)
                                      }
                                    >
                                      {settingKeyLabel(group.setting)}
                                    </button>
                                  </td>
                                  <td rowSpan={group.settings.length}>
                                    {meta
                                      ? formatConfidentiality(
                                          meta.confidentiality
                                        )
                                      : "—"}
                                  </td>
                                  <td rowSpan={group.settings.length}>
                                    {meta
                                      ? formatPermission(meta.readPermission)
                                      : "—"}
                                  </td>
                                  <td rowSpan={group.settings.length}>
                                    {meta
                                      ? formatPermission(meta.writePermission)
                                      : "—"}
                                  </td>
                                  <td rowSpan={group.settings.length}>
                                    {formatConstraints(meta)}
                                  </td>
                                </>
                              )}
                              <td>{formatSource(groupSetting.source)}</td>
                              <td>
                                {groupSetting.value
                                  ? formatValue(groupSetting.value)
                                  : "(hidden)"}
                              </td>
                              <td>
                                {groupSetting.hasUnsavedValue ? "yes" : "no"}
                              </td>
                            </tr>
                          ))}
                          {isSelected && (
                            <tr className="settings-editor-row">
                              <td colSpan={8}>{updateValuePanel}</td>
                            </tr>
                          )}
                        </Fragment>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            </div>
          ))
        ) : (
          <p className="empty-message">
            Settings load automatically when the device connects. Use List to
            reload the current filter.
          </p>
        )}
      </div>

      <div className="json-panel">
        <label htmlFor="settings-json">Settings JSON</label>
        <textarea
          id="settings-json"
          rows={12}
          value={jsonText}
          onChange={(e) => setJsonText(e.target.value)}
        />
        <div className="toolbar">
          <button className="btn" disabled={isLoading} onClick={exportJson}>
            Export JSON
          </button>
          <button
            className="btn btn-primary"
            disabled={isLoading || jsonText.trim().length === 0}
            onClick={importJson}
          >
            Import JSON
          </button>
        </div>
      </div>

      {setting && (
        <dl className="setting-summary">
          <div>
            <dt>Setting</dt>
            <dd>
              {subsystemIdentifierForIndex(setting.customSubsystemIndex) ??
                setting.customSubsystemIndex}
              /{setting.key}
            </dd>
          </div>
          <div>
            <dt>Array</dt>
            <dd>
              {setting.value?.arrayValue
                ? `${setting.value.arrayValue.index}/${setting.value.arrayValue.size}`
                : "no"}
            </dd>
          </div>
          <div>
            <dt>Source</dt>
            <dd>{formatSource(editorSource)}</dd>
          </div>
          <div>
            <dt>Unsaved</dt>
            <dd>{setting.hasUnsavedValue ? "yes" : "no"}</dd>
          </div>
          {setting.defaultValue && (
            <div>
              <dt>Default</dt>
              <dd>{formatValue(setting.defaultValue)}</dd>
            </div>
          )}
        </dl>
      )}

      {response && (
        <div className="response-box">
          <pre>{response}</pre>
        </div>
      )}
    </section>
  );
}

function formatSetting(setting: Setting): string {
  const value = setting.value ? formatValue(setting.value) : "(hidden)";
  const suffix = setting.defaultValue
    ? ` (default ${formatValue(setting.defaultValue)})`
    : "";
  return `${setting.customSubsystemIndex}/${setting.key} = ${value}${suffix}`;
}

function formatValue(value: SettingValue): string {
  if (value.arrayValue !== undefined) {
    return `[${value.arrayValue.index}/${value.arrayValue.size}] ${formatScalarValue(value.arrayValue.value ?? {})}`;
  }

  return formatScalarValue(value);
}

function settingValueTypeLabel(
  value: SettingValue | undefined
): string | undefined {
  const scalarValue = value?.arrayValue?.value ?? value;
  if (!scalarValue) return undefined;
  if (scalarValue.int32Value !== undefined) return "int32";
  if (scalarValue.boolValue !== undefined) return "bool";
  if (scalarValue.stringValue !== undefined) return "string";
  if (scalarValue.bytesValue !== undefined) return "bytes";
  if (scalarValue.behaviorValue !== undefined) return "behavior";
  return undefined;
}

function formatScalarValue(value: SettingScalarValue): string {
  if (value.int32Value !== undefined) return `${value.int32Value}`;
  if (value.boolValue !== undefined) return value.boolValue ? "true" : "false";
  if (value.stringValue !== undefined) return value.stringValue;
  if (value.bytesValue !== undefined) {
    return formatBytesValue(value.bytesValue);
  }
  if (value.behaviorValue !== undefined) {
    return formatBehaviorValue(value.behaviorValue);
  }
  return "";
}

function parseBehaviorValue(value: string): {
  behaviorId: number;
  param1: number;
  param2: number;
} {
  const [behaviorId = 0, param1 = 0, param2 = 0] = value
    .split(",")
    .map((token) => Number.parseInt(token.trim(), 10) || 0);
  return { behaviorId, param1, param2 };
}

function formatBehaviorValue(value: {
  behaviorId: number;
  param1: number;
  param2: number;
}): string {
  return `${value.behaviorId},${value.param1},${value.param2}`;
}

function parseBytesValue(value: string): Uint8Array {
  const trimmed = value.trim();
  if (trimmed.length === 0) return new Uint8Array();

  const tokens = trimmed.split(/[\s,]+/).filter(Boolean);
  if (
    tokens.length > 0 &&
    tokens.every((token) => /^[0-9a-fA-F]{1,2}$/.test(token))
  ) {
    return Uint8Array.from(tokens.map((token) => Number.parseInt(token, 16)));
  }

  return new TextEncoder().encode(value);
}

function formatBytesValue(value: Uint8Array): string {
  return Array.from(value)
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join(" ");
}

function parseScalarValueOf(
  type: EditorValueType,
  raw: string
): SettingScalarValue {
  switch (type) {
    case EditorValueType.Bytes:
      return { bytesValue: parseBytesValue(raw) };
    case EditorValueType.Bool:
      return { boolValue: raw === "true" || raw === "1" };
    case EditorValueType.String:
      return { stringValue: raw };
    case EditorValueType.Behavior:
      return { behaviorValue: parseBehaviorValue(raw) };
    case EditorValueType.Int32:
    default:
      return { int32Value: Number.parseInt(raw, 10) || 0 };
  }
}

function settingGroupKey(setting: Setting): string {
  const arrayIndex = setting.value?.arrayValue?.index ?? "scalar";
  return `${setting.customSubsystemIndex}:${setting.key}:${arrayIndex}`;
}

function groupSettings(settings: Setting[]): SettingGroup[] {
  const groups = new Map<string, SettingGroup>();

  for (const setting of settings) {
    const key = settingGroupKey(setting);
    const group = groups.get(key);
    if (group) {
      group.settings.push(setting);
    } else {
      groups.set(key, { key, setting, settings: [setting] });
    }
  }

  return Array.from(groups.values()).map((group) => {
    const sortedSettings = sortSettingsBySource(group.settings);
    return {
      ...group,
      setting: sortedSettings[0],
      settings: sortedSettings,
    };
  });
}

/**
 * Buckets already-grouped settings by their custom subsystem so each subsystem
 * renders its own narrower table. Insertion order is preserved.
 */
function groupBySubsystem(groups: SettingGroup[]): SubsystemGroup[] {
  const bySubsystem = new Map<number, SettingGroup[]>();

  for (const group of groups) {
    const index = group.setting.customSubsystemIndex;
    const existing = bySubsystem.get(index);
    if (existing) {
      existing.push(group);
    } else {
      bySubsystem.set(index, [group]);
    }
  }

  return Array.from(bySubsystem.entries()).map(
    ([customSubsystemIndex, subsystemGroups]) => ({
      customSubsystemIndex,
      groups: subsystemGroups,
    })
  );
}

function sortSettingsBySource(settings: Setting[]): Setting[] {
  return [...settings].sort(
    (a, b) => sourceSortValue(a.source) - sourceSortValue(b.source)
  );
}

function sourceOptionsForSetting(
  setting: Setting,
  listedSettings: Setting[]
): number[] {
  const sources = new Set<number>([setting.source, SOURCE_ALL]);
  for (const listedSetting of listedSettings) {
    if (sameSettingIdentity(setting, listedSetting)) {
      sources.add(listedSetting.source);
    }
  }

  return Array.from(sources).sort(
    (a, b) => sourceSortValue(a) - sourceSortValue(b)
  );
}

function sourceSortValue(source: number): number {
  if (source === SOURCE_LOCAL) return -1;
  if (source === SOURCE_ALL) return Number.MAX_SAFE_INTEGER;
  return source;
}

function sameSettingIdentity(a: Setting, b: Setting): boolean {
  return (
    a.customSubsystemIndex === b.customSubsystemIndex &&
    a.key === b.key &&
    (a.value?.arrayValue?.index ?? undefined) ===
      (b.value?.arrayValue?.index ?? undefined)
  );
}

/**
 * The setting's key plus an array suffix, without the subsystem prefix. Used in
 * the per-subsystem tables where the subsystem already appears as the heading.
 */
function settingKeyLabel(setting: Setting): string {
  const arraySuffix =
    setting.value?.arrayValue !== undefined
      ? `[${setting.value.arrayValue.index}]`
      : "";
  return `${setting.key}${arraySuffix}`;
}

function formatConfidentiality(value: SettingConfidentiality): string {
  switch (value) {
    case SettingConfidentiality.SETTING_CONFIDENTIALITY_DEVICE_PRIVATE:
      return "device-private";
    case SettingConfidentiality.SETTING_CONFIDENTIALITY_RPC_PERSONAL:
      return "personal";
    case SettingConfidentiality.SETTING_CONFIDENTIALITY_RPC_PUBLIC:
      return "public";
    default:
      return "?";
  }
}

function formatPermission(value: SettingPermission): string {
  return value === SettingPermission.SETTING_PERMISSION_SECURE
    ? "secure"
    : "unsecure";
}

function formatConstraints(meta: SettingMeta | undefined): string {
  if (!meta || meta.constraints.length === 0) {
    return "—";
  }
  const parts = meta.constraints.map(formatConstraint).filter(Boolean);
  return parts.length > 0 ? parts.join(", ") : "—";
}

function formatConstraint(constraint: SettingConstraint): string {
  switch (constraintKind(constraint)) {
    case "range": {
      const min = constraint.range?.min
        ? scalarValueToEditorString(constraint.range.min)
        : "?";
      const max = constraint.range?.max
        ? scalarValueToEditorString(constraint.range.max)
        : "?";
      return `range ${min}–${max}`;
    }
    case "options":
      return `options(${constraint.options?.values.length ?? 0})`;
    case "hidUsage":
      return `hid usage p${constraint.hidUsage?.usagePage ?? 0}`;
    case "layerId":
      return "layer";
    case "behaviorId":
      return "behavior";
    default:
      return "";
  }
}

function settingDisplayName(
  setting: Setting,
  subsystemIdentifierForIndex: (index: number) => string | undefined
): string {
  const subsystem =
    subsystemIdentifierForIndex(setting.customSubsystemIndex) ??
    `${setting.customSubsystemIndex}`;
  const arraySuffix =
    setting.value?.arrayValue !== undefined
      ? `[${setting.value.arrayValue.index}]`
      : "";
  return `${subsystem}/${setting.key}${arraySuffix}`;
}

function formatSource(source: number): string {
  if (source === 0) return "local";
  if (source === SOURCE_ALL) return "all";
  return `${source}`;
}

function optionalString(value: string): string | undefined {
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

function scopeTargetsAll(scope: SettingScope | undefined): boolean {
  return scope?.source === SOURCE_ALL;
}

async function withTimeout<T>(
  promise: Promise<T>,
  timeoutMs: number,
  message: string
): Promise<T> {
  let timeout: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      promise,
      new Promise<never>((_, reject) => {
        timeout = setTimeout(() => reject(new Error(message)), timeoutMs);
      }),
    ]);
  } finally {
    if (timeout) {
      clearTimeout(timeout);
    }
  }
}

export default App;
