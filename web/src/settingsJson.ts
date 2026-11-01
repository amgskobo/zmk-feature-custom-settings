import {
  Setting,
  SettingScalarValue,
  SettingValue,
} from "./proto/cormoran/zmk/custom_settings/custom_settings";

const SETTINGS_EXPORT_FORMAT = "zmk-custom-settings";
const SETTINGS_EXPORT_VERSION = 1;

type ExportedSettingType = "bytes" | "int32" | "bool" | "string" | "behavior";

export interface ExportedSetting {
  customSubsystemId: string;
  key: string;
  source: number;
  type: ExportedSettingType;
  value: boolean | number | string | number[];
  arrayIndex?: number;
  arraySize?: number;
}

export interface ExportedSettingEntry {
  type: ExportedSettingType;
  source: number;
  value: ExportedSetting["value"] | ExportedSetting["value"][];
  size?: number;
}

export interface SettingsExportDocument {
  format: typeof SETTINGS_EXPORT_FORMAT;
  version: typeof SETTINGS_EXPORT_VERSION;
  exportedAt: string;
  customSubsystems: Record<
    string,
    Record<string, ExportedSettingEntry | ExportedSettingEntry[]>
  >;
}

export function countExportedSettings(doc: SettingsExportDocument): number {
  return Object.values(doc.customSubsystems).reduce(
    (sum, settings) =>
      sum +
      Object.values(settings).reduce(
        (s, value) =>
          s +
          (Array.isArray(value) ? value : [value]).reduce(
            (count, entry) => count + (entry.size ?? 1),
            0
          ),
        0
      ),
    0
  );
}

export function settingToExportedSetting(
  setting: Setting,
  subsystemIdentifierForIndex: (index: number) => string | undefined = (
    index
  ) => String(index)
): ExportedSetting | null {
  if (!setting.value) {
    return null;
  }

  const scalarValue = setting.value.arrayValue?.value ?? setting.value;
  const type = scalarValueType(scalarValue);
  if (!type) {
    return null;
  }

  const value = scalarValueToJsonValue(type, scalarValue);
  if (value === undefined) {
    return null;
  }

  const customSubsystemId = subsystemIdentifierForIndex(
    setting.customSubsystemIndex
  );
  if (!customSubsystemId) {
    return null;
  }

  const exported: ExportedSetting = {
    customSubsystemId,
    key: setting.key,
    source: setting.source,
    type,
    value,
  };

  if (setting.value.arrayValue) {
    exported.arrayIndex = setting.value.arrayValue.index;
    exported.arraySize = setting.value.arrayValue.size;
  }

  return exported;
}

export function createSettingsExportDocument(
  settings: Setting[],
  subsystemIdentifierForIndex?: (index: number) => string | undefined
): SettingsExportDocument {
  const allExported: ExportedSetting[] = [];
  for (const setting of settings) {
    const exported = settingToExportedSetting(
      setting,
      subsystemIdentifierForIndex
    );
    if (exported) allExported.push(exported);
  }

  const bySubsystem = new Map<
    string,
    Map<string, Map<number, ExportedSetting[]>>
  >();
  for (const s of allExported) {
    let byKey = bySubsystem.get(s.customSubsystemId);
    if (!byKey) {
      byKey = new Map();
      bySubsystem.set(s.customSubsystemId, byKey);
    }
    let bySource = byKey.get(s.key);
    if (!bySource) {
      bySource = new Map();
      byKey.set(s.key, bySource);
    }
    const entries = bySource.get(s.source) ?? [];
    entries.push(s);
    bySource.set(s.source, entries);
  }

  const customSubsystems: SettingsExportDocument["customSubsystems"] =
    Object.create(null);
  for (const [subsystemId, byKey] of bySubsystem) {
    customSubsystems[subsystemId] = Object.create(null);
    for (const [key, bySource] of byKey) {
      const sources: ExportedSettingEntry[] = [];
      for (const entries of bySource.values()) {
        const first = entries[0];
        if (first.arrayIndex !== undefined) {
          const size = first.arraySize!;
          const valueArray: ExportedSetting["value"][] = new Array(size);
          for (const entry of entries) {
            valueArray[entry.arrayIndex!] = entry.value;
          }
          sources.push({
            type: first.type,
            source: first.source,
            size,
            value: valueArray,
          });
        } else {
          sources.push({
            type: first.type,
            source: first.source,
            value: first.value,
          });
        }
      }
      // Keep the existing single-source representation readable by older
      // clients; repeated sources use separate complete entries for this key.
      customSubsystems[subsystemId][key] =
        sources.length === 1 ? sources[0] : sources;
    }
  }

  return {
    format: SETTINGS_EXPORT_FORMAT,
    version: SETTINGS_EXPORT_VERSION,
    exportedAt: new Date().toISOString(),
    customSubsystems,
  };
}

export function settingsExportToJson(
  settings: Setting[],
  subsystemIdentifierForIndex?: (index: number) => string | undefined
): string {
  return JSON.stringify(
    createSettingsExportDocument(settings, subsystemIdentifierForIndex),
    null,
    2
  );
}

export function parseSettingsExportJson(json: string): ExportedSetting[] {
  const parsed: unknown = JSON.parse(json);

  if (!isRecord(parsed)) {
    throw new Error("Invalid JSON: expected an object");
  }
  if (parsed.format !== SETTINGS_EXPORT_FORMAT) {
    throw new Error(`Invalid format: expected "${SETTINGS_EXPORT_FORMAT}"`);
  }
  if (parsed.version !== SETTINGS_EXPORT_VERSION) {
    throw new Error(
      `Unsupported version ${parsed.version}, expected ${SETTINGS_EXPORT_VERSION}`
    );
  }
  if (!isRecord(parsed.customSubsystems)) {
    throw new Error("JSON must contain a customSubsystems object");
  }

  const result: ExportedSetting[] = [];
  for (const [customSubsystemId, subsystemEntries] of Object.entries(
    parsed.customSubsystems
  )) {
    if (customSubsystemId.length === 0) {
      throw new Error("customSubsystems contains an empty key");
    }
    if (!isRecord(subsystemEntries)) {
      throw new Error(
        `customSubsystems.${customSubsystemId} must be an object`
      );
    }
    for (const [key, entry] of Object.entries(subsystemEntries)) {
      if (key.length === 0) {
        throw new Error(
          `customSubsystems.${customSubsystemId} contains an empty key`
        );
      }
      const label = `customSubsystems.${customSubsystemId}.${key}`;
      const sources = Array.isArray(entry) ? entry : [entry];
      const seen = new Set<number>();
      for (const source of sources) {
        const settings = parseSettingEntry(
          source,
          customSubsystemId,
          key,
          label
        );
        const id = settings[0]?.source;
        if (id !== undefined && seen.has(id)) {
          throw new Error(`${label} contains a duplicate source`);
        }
        if (id !== undefined) seen.add(id);
        result.push(...settings);
      }
    }
  }
  return result;
}

export function exportedSettingValueToProto(
  setting: ExportedSetting
): SettingValue {
  const scalarValue = exportedScalarValueToProto(setting);
  if (setting.arrayIndex !== undefined) {
    if (setting.arraySize === undefined) {
      throw new Error(
        `${setting.customSubsystemId}/${setting.key} is missing arraySize`
      );
    }

    return {
      arrayValue: {
        index: setting.arrayIndex,
        size: setting.arraySize,
        value: scalarValue,
      },
    };
  }

  return scalarValue;
}

function scalarValueType(
  value: SettingScalarValue
): ExportedSettingType | null {
  if (value.bytesValue !== undefined) return "bytes";
  if (value.int32Value !== undefined) return "int32";
  if (value.boolValue !== undefined) return "bool";
  if (value.stringValue !== undefined) return "string";
  if (value.behaviorValue !== undefined) return "behavior";
  return null;
}

function scalarValueToJsonValue(
  type: ExportedSettingType,
  value: SettingScalarValue
): ExportedSetting["value"] | undefined {
  switch (type) {
    case "bytes":
      return value.bytesValue ? Array.from(value.bytesValue) : undefined;
    case "int32":
      return value.int32Value;
    case "bool":
      return value.boolValue;
    case "string":
      return value.stringValue;
    case "behavior":
      return value.behaviorValue
        ? [
            value.behaviorValue.behaviorId,
            value.behaviorValue.param1,
            value.behaviorValue.param2,
          ]
        : undefined;
  }
}

function exportedScalarValueToProto(setting: ExportedSetting): SettingValue {
  switch (setting.type) {
    case "bytes":
      if (!Array.isArray(setting.value)) {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} bytes value must be an array`
        );
      }
      return { bytesValue: Uint8Array.from(setting.value) };
    case "int32":
      if (typeof setting.value !== "number") {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} int32 value must be a number`
        );
      }
      return { int32Value: setting.value };
    case "bool":
      if (typeof setting.value !== "boolean") {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} bool value must be a boolean`
        );
      }
      return { boolValue: setting.value };
    case "string":
      if (typeof setting.value !== "string") {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} string value must be a string`
        );
      }
      return { stringValue: setting.value };
    case "behavior": {
      if (!Array.isArray(setting.value) || setting.value.length !== 3) {
        throw new Error(
          `${setting.customSubsystemId}/${setting.key} behavior value must be a [behaviorId, param1, param2] array`
        );
      }
      const [behaviorId, param1, param2] = setting.value as number[];
      return { behaviorValue: { behaviorId, param1, param2 } };
    }
  }
}

function parseSettingEntry(
  entry: unknown,
  customSubsystemId: string,
  key: string,
  label: string
): ExportedSetting[] {
  if (!isRecord(entry)) {
    throw new Error(`${label} must be an object`);
  }

  const type = readSettingType(entry.type, label);
  const source = readNonNegativeInteger(entry.source, "source", label);

  if (entry.size !== undefined) {
    const size = readPositiveInteger(entry.size, "size", label);
    if (!Array.isArray(entry.value) || entry.value.length !== size) {
      throw new Error(`${label}.value must be an array of length ${size}`);
    }
    const valueArr = entry.value as unknown[];
    return Array.from({ length: size }, (_, i) => ({
      customSubsystemId,
      key,
      source,
      type,
      value: readSettingValue(valueArr[i], type, `${label}[${i}]`),
      arrayIndex: i,
      arraySize: size,
    }));
  }

  return [
    {
      customSubsystemId,
      key,
      source,
      type,
      value: readSettingValue(entry.value, type, label),
    },
  ];
}

function readSettingType(value: unknown, label: string): ExportedSettingType {
  if (
    value === "bytes" ||
    value === "int32" ||
    value === "bool" ||
    value === "string" ||
    value === "behavior"
  ) {
    return value;
  }

  throw new Error(`${label}.type is not supported`);
}

function readSettingValue(
  value: unknown,
  type: ExportedSettingType,
  label: string
): ExportedSetting["value"] {
  switch (type) {
    case "bytes":
      if (
        !Array.isArray(value) ||
        value.some((byte) => !Number.isInteger(byte) || byte < 0 || byte > 255)
      ) {
        throw new Error(`${label}.value must be a byte array`);
      }
      return value as number[];
    case "int32":
      if (!Number.isInteger(value)) {
        throw new Error(`${label}.value must be an integer`);
      }
      return value as number;
    case "bool":
      if (typeof value !== "boolean") {
        throw new Error(`${label}.value must be a boolean`);
      }
      return value;
    case "string":
      if (typeof value !== "string") {
        throw new Error(`${label}.value must be a string`);
      }
      return value;
    case "behavior":
      if (
        !Array.isArray(value) ||
        value.length !== 3 ||
        value.some((n) => !Number.isInteger(n) || n < 0)
      ) {
        throw new Error(
          `${label}.value must be a [behaviorId, param1, param2] array of non-negative integers`
        );
      }
      return value as number[];
  }
}

function readNonNegativeInteger(
  value: unknown,
  field: string,
  label: string
): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value < 0) {
    throw new Error(`${label}.${field} must be a non-negative integer`);
  }

  return value;
}

function readPositiveInteger(
  value: unknown,
  field: string,
  label: string
): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value <= 0) {
    throw new Error(`${label}.${field} must be a positive integer`);
  }

  return value;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
