# Toshiba control matrix redesign

## Purpose

This document defines the control model for Toshiba indoor units before further Home Assistant / ESPHome UI changes are made.

The governing order is:

1. **Toshiba remote control / indoor-unit panel** — canonical user-facing control taxonomy.
2. **Toshiba operation and service manuals** — valid combinations, restrictions, forced side effects and cancellation behaviour.
3. **UART protocol** — implementation mechanism used to read and command those controls.
4. **ESPHome entities** — expose the logical controls without inheriting UART register grouping.
5. **Home Assistant** — present the resulting entities; Toshiba-specific behaviour must not be implemented in HA automations merely to compensate for a bad device model.

A UART register must not be treated as a UI dimension merely because several Toshiba commands share that register.

---

## Target units

Current target installation:

| Location | Indoor unit | Family | Nominal class |
|---|---|---|---:|
| Kitchen | RAS-B13G3KVSG-E | G3KVSG high-wall | 13 |
| Music / Hall | RAS-B13J2FVG-E | J2FVG bi-flow console | 13 |
| Office | RAS-B10J2FVG-E | J2FVG bi-flow console | 10 |
| Dining | RAS-B10J2FVG-E | J2FVG bi-flow console | 10 |

The outdoor unit is RAS-5M34G3AVG-E.

The Music / Hall console and Kitchen wall unit are currently the preferred reference units for protocol work because they expose the richer/newer UART behaviour seen so far. The Office and Dining consoles must be treated separately where their older firmware differs.

---

## Design rule: remote control first

The Toshiba remote/panel controls are the closest available specification of the intended user model. Where the remote gives two functions separate buttons or separate menu entries, the ESPHome component should presume they are independent logical controls unless Toshiba documentation or UART testing proves otherwise.

Examples:

- ECO and FLOOR are separate controls on the J2FVG family.
- AIR OUTLET SELECT is separate from FLOOR.
- Power Select is independent of ECO / Hi POWER.
- Fireplace and Outdoor Silent are multi-state functions of their own.
- Fixed louvre position is not equivalent to swing.

The current `supported_presets` abstraction violates this rule because it presents multiple independent Toshiba functions as one mutually exclusive Home Assistant preset dimension.

---

## Home Assistant division of duty

The primary climate entity should contain only genuine climate dimensions:

| Logical function | HA / ESPHome representation |
|---|---|
| Power | climate entity |
| HVAC mode | `hvac_mode` |
| Target temperature | climate target temperature |
| Fan speed | `fan_mode` |
| Vertical swing | `swing_mode` |
| Horizontal swing | `swing_horizontal_mode` where supported |

Manufacturer-specific functions should normally be separate entities:

| Toshiba function | Proposed entity type |
|---|---|
| ECO | switch |
| Hi POWER | switch |
| Power Select | select: 100 / 75 / 50 |
| Outdoor Silent | select: Off / Silent 1 / Silent 2 |
| Fireplace | select: Off / Fireplace 1 / Fireplace 2 |
| FLOOR warming | switch |
| Air Outlet | select |
| Fixed vertical louvre | select |
| Fixed horizontal louvre | select if supported |
| HADA Care | separate airflow control |
| Comfort Sleep | separate feature entity; exact type TBD from UART semantics |
| Self Clean | binary status sensor unless command support is proven |

`8 °C` operation should be modelled from Toshiba's documented low-temperature Heat behaviour rather than automatically exposed as a peer preset.

---

# 1. Physical-control capability matrix

Status meanings:

- **YES** — clearly part of the model family's documented user interface.
- **NO** — not part of that model family's documented interface.
- **TBD** — must be verified against the exact manual / firmware.

| Control | G3KVSG wall | J2FVG console | Notes |
|---|---:|---:|---|
| ON/OFF | YES | YES | Primary climate power |
| MODE | YES | YES | Auto / Cool / Heat / Dry / Fan subject to exact model documentation |
| TEMP | YES | YES | Primary climate target |
| FAN | YES | YES | Toshiba fan-level sequence must be preserved |
| Vertical FIX | YES | YES | Fixed vertical louvre position |
| Vertical SWING | YES | YES | Separate from fixed position |
| Horizontal FIX | YES | NO/TBD | Wall-family feature; verify exact G3KVSG behaviour |
| Horizontal SWING | YES | NO/TBD | Wall-family feature; verify exact G3KVSG behaviour |
| ECO | YES | YES | Independent control |
| Hi POWER | YES | YES | Independent control |
| Power Select | YES | YES | 100 / 75 / 50 |
| Outdoor Silent | YES | YES | Off / Silent 1 / Silent 2 |
| Fireplace | YES | YES/TBD | Verify exact console manual behaviour |
| Comfort Sleep | YES | YES | Independent feature |
| 8 °C / frost heat | YES | YES | Treat as Heat semantics / restricted operating range |
| FLOOR warming | NO | YES | Heat-specific console function |
| AIR OUTLET SELECT | NO | YES | Console panel function; not the same as FLOOR |
| HADA Care | YES | NO | Wall airflow function |
| Self Clean | YES/TBD | YES/TBD | Currently report-only in component |

---

# 2. Proposed logical-control matrix

This table is the specification the implementation should eventually satisfy.

| Logical function | States | Applies to | Proposed HA entity | Current UART mapping | Read | Write | Notes |
|---|---|---|---|---|---|---|---|
| HVAC mode | Auto/Cool/Heat/Dry/Fan | Both | climate | `MODE` 0xB0 | YES | YES | Existing implementation |
| Target temp | model range | Both | climate | `TARGET_TEMP` 0xB3 | YES | YES | 8 °C behaviour needs special handling |
| Fan mode | Auto/Quiet/Low/Low+/Med/Med+/High | Both | climate fan mode | `FAN` 0xA0 | YES | YES | Rename intermediate levels to Toshiba terminology |
| Vertical swing | Off/On | Both | climate swing | `SWING` 0xA3 | YES | YES | Separate from fixed positions |
| Horizontal swing | Off/On | G3KVSG | horizontal swing | `SWING` 0xA3 | YES/TBD | YES/TBD | Test exact wall behaviour |
| Vertical fixed position | Toshiba positions | Both | select | `SWING` fixed codes | YES | YES | Existing separate select is conceptually correct |
| Horizontal fixed position | Toshiba positions | G3KVSG | select | TBD | TBD | TBD | Requires targeted UART test |
| ECO | Off/On | Both | switch | currently SPECIAL_MODE 0xF7 value 3 | YES/TBD | YES | Must no longer be a preset peer of FLOOR etc. |
| Hi POWER | Off/On | Both | switch | currently SPECIAL_MODE 0xF7 value 1 | YES/TBD | YES | Dependency/cancellation behaviour TBD |
| FLOOR warming | Off/On | J2FVG | switch | currently SPECIAL_MODE 0xF7 value 6 | YES/TBD | YES | Heat-only; forces other state changes |
| Air Outlet | mode-dependent | J2FVG | select | UNKNOWN | UNKNOWN | UNKNOWN | Highest-priority missing protocol function |
| Fireplace | Off/1/2 | supported units | select | 0xF7 values 32/48 currently | YES/TBD | YES/TBD | Treat as independent multi-state function |
| Outdoor Silent | Off/1/2 | supported units | select | 0xF7 values 2/10 currently labelled Silent | YES/TBD | YES/TBD | Confirm these values really represent ODU Silent state |
| Power Select | 100/75/50 | Both | select | `POWER_SEL` 0x87 | YES | YES | Existing abstraction is good |
| Comfort Sleep | Off/active/timed state TBD | Both | TBD | `COMFORT_SLEEP` 0x94 exists; 0xF7 Sleep also exists | TBD | TBD | Current code model is internally inconsistent; investigate |
| 8 °C heat | Heat target / restricted range | Both | climate behaviour | currently 0xF7 value 4 + temp workaround | YES/TBD | YES/TBD | Do not expose blindly as ordinary preset |
| HADA Care | Off/On or airflow state TBD | G3KVSG | separate control | UNKNOWN | UNKNOWN | UNKNOWN | Targeted wall-unit test required |
| Self Clean | Off/Running | Both | binary sensor | `SELF_CLEAN` 0xCB | YES | not implemented | Preserve as status unless control is proven |

`Read` and `Write` above describe protocol knowledge, not physical capability. Values marked TBD/UNKNOWN must not be promoted to full support without evidence.

---

# 3. Behaviour / dependency matrix

This is the critical part of the redesign. Controls may remain logically distinct while still having side effects or conflicts.

Every function must eventually have the following fields established:

- valid HVAC modes;
- state forced by Toshiba when activated;
- controls inhibited while active;
- what happens if the user presses a conflicting control;
- whether disabling the function restores a previous state or leaves Toshiba's resulting state in place;
- whether old and new firmware behave differently.

Initial matrix:

| Function / state | Valid mode(s) | Known or suspected forced state | Known / suspected conflict | Conflicting-control behaviour | Evidence status |
|---|---|---|---|---|---|
| FLOOR ON | Heat | Fan = Auto; downward/floor discharge behaviour | manual Fan; Swing | TBD: reject, cancel FLOOR, or Toshiba changes state | Partially observed + manual; complete test required |
| ECO ON | TBD by mode | Toshiba internally modifies demand | Hi POWER likely incompatible | TBD | Manual + test required |
| Hi POWER ON | TBD by mode | Toshiba internally modifies demand / airflow | ECO likely incompatible | TBD | Manual + test required |
| Air Outlet = Lower | Heat expected | Lower outlet | modes that do not permit lower-only | TBD | Manual rule; UART unknown |
| Air Outlet = Upper+Lower | Heat/Cool where allowed | both outlets | mode-specific restrictions | TBD | Manual rule; UART unknown |
| 8 °C Heat | Heat only | restricted heating behaviour | Quiet / Hi POWER and possibly others | TBD | Manual restriction; exact UART behaviour TBD |
| Comfort Sleep | supported heating/cooling modes TBD | Toshiba controls temperature/airflow over time | TBD | TBD | Manual + UART test required |
| Outdoor Silent | system-level effect | ODU limitation | TBD | TBD | Verify 0xF7 semantics |
| Power Select 75/50 | supported modes | ODU/input limitation | Hi POWER interaction TBD | TBD | Existing UART works; interaction testing required |

Do not encode a dependency merely from an assumption. A rule becomes implementation logic only after manual or repeatable device evidence establishes it.

---

# 4. Firmware capability model

Physical model capability and UART capability must be separate concepts.

Recommended protocol-support states:

```cpp
enum class Capability {
  UNSUPPORTED,
  READ_ONLY,
  WRITE_ONLY,
  READ_WRITE,
  UNKNOWN
};
```

Example conceptual result for an older J2FVG firmware:

```text
FLOOR
  physical: YES
  uart: READ_WRITE

AIR OUTLET
  physical: YES
  uart: UNKNOWN

ECO
  physical: YES
  uart: WRITE_ONLY
```

This allows the component to describe reality without pretending that every documented control is equally visible over UART.

The Music / Hall J2FVG and Kitchen G3KVSG should initially serve as reference implementations. Office and Dining must then be tested against the same matrix rather than assumed identical merely because the model family is the same.

---

# 5. State authority and side effects

Toshiba firmware is authoritative for the actual resulting operating state.

Example:

1. Home Assistant requests `FLOOR = ON`.
2. ESPHome sends the Toshiba FLOOR command.
3. Toshiba changes FLOOR state and may force Fan = Auto and alter outlet/louvre behaviour.
4. ESPHome reads/publishes the resulting Toshiba states.

ESPHome should **not** independently send `FAN=AUTO` just because the dependency matrix says FLOOR normally causes that, unless UART tracing proves that Toshiba's own remote issues multiple commands and those commands are required.

Likewise, when leaving FLOOR, ESPHome must not restore an earlier manual fan setting unless Toshiba itself does so or the documented remote behaviour requires it.

This keeps device semantics separate from higher-level control policy.

---

# 6. Conflict handling

For each conflicting combination, determine how the physical Toshiba control behaves.

Possible behaviours include:

1. the conflicting command is ignored;
2. the existing special function is cancelled and the new command takes effect;
3. the new command is accepted but coerced to another state;
4. both controls remain valid because the assumed conflict was false.

ESPHome should emulate the Toshiba control semantics, not invent an alternative policy.

A useful acceptance test is:

> Can Home Assistant produce every meaningful combination that the Toshiba remote/panel can produce, and does it resolve invalid combinations in the same way as the Toshiba controls?

---

# 7. Required UART test programme

Testing should be control-driven rather than register-driven.

For each reference unit:

1. Establish a stable baseline state.
2. Record all currently readable relevant UART state.
3. Press exactly one Toshiba remote/panel control.
4. Capture all UART traffic / changed registers.
5. Record resulting physical behaviour and all secondary state changes.
6. Press any known conflicting control and record what happens.
7. Return to baseline.
8. Repeat at least once to distinguish deterministic behaviour from transient traffic.

Priority order:

### J2FVG Music / Hall reference

1. AIR OUTLET SELECT in Heat: Upper+Lower -> Upper -> Lower.
2. AIR OUTLET SELECT in Cool.
3. AIR OUTLET behaviour in Dry.
4. FLOOR ON/OFF with fan deliberately set manually before activation.
5. While FLOOR is active, request manual Fan and Swing from the remote.
6. ECO ON/OFF and interaction with Hi POWER.
7. Power Select interaction with Hi POWER / ECO.
8. Comfort Sleep.
9. Fireplace / 8 °C behaviour.
10. Outdoor Silent.

### G3KVSG Kitchen reference

1. Vertical fixed position and swing.
2. Horizontal fixed position and swing independently.
3. Combined swing.
4. HADA Care.
5. ECO / Hi POWER conflict behaviour.
6. Fireplace.
7. Outdoor Silent.
8. Power Select interactions.
9. 8 °C heat restrictions.
10. Comfort Sleep.

### Older J2FVG Office / Dining

Repeat the J2FVG matrix only after the reference console behaviour is characterised. Record each feature as READ_WRITE, READ_ONLY, WRITE_ONLY, UNSUPPORTED or UNKNOWN.

The purpose is not merely to find additional registers. It is to determine where the older firmware divides responsibility differently or withholds state that the newer firmware reports.

---

# 8. Proposed code architecture

The current `SPECIAL_MODE` enum may remain as a protocol decoder temporarily, but must no longer define the public logical state model.

Proposed conceptual internal state:

```cpp
struct ToshibaLogicalState {
  HVACMode hvac_mode;
  float target_temperature;
  FanMode fan_mode;

  bool eco;
  bool hi_power;
  bool floor;

  FireplaceMode fireplace;
  OutdoorSilentMode outdoor_silent;
  PowerLevel power_level;
  AirOutletMode air_outlet;

  VerticalLouvreMode vertical_louvre;
  bool vertical_swing;

  HorizontalLouvreMode horizontal_louvre;
  bool horizontal_swing;

  ComfortSleepState comfort_sleep;
  bool self_clean;
};
```

Not every model will populate every member.

The architecture should separate:

```text
UART packet
   ↓
protocol decoder
   ↓
logical Toshiba state
   ↓
capability + dependency rules
   ↓
ESPHome entities
   ↓
Home Assistant
```

Do not allow this shortcut again:

```text
UART register
   ↓
Home Assistant UI selector
```

---

# 9. Configuration direction

The existing free-form `supported_presets` configuration should be deprecated once independent feature entities exist.

Target direction:

```yaml
climate:
  - platform: toshiba_suzumi
    name: Music Room
    uart_id: uart_bus
    model: j2fvg

    eco:
      name: "ECO"

    hi_power:
      name: "Hi POWER"

    floor:
      name: "Floor warming"

    air_outlet:
      name: "Air outlet"

    fireplace:
      name: "Fireplace"

    outdoor_silent:
      name: "Outdoor silent"

    power_select:
      name: "Power level"

    vertical_air_direction:
      name: "Vertical air direction"
```

Longer term, `model: j2fvg` / `model: g3kvsg` may create the appropriate standard entities automatically, while individual YAML entries remain available for naming or disabling entities.

Do not expose physically impossible functions merely because their UART code exists in the generic component.

---

# 10. Migration strategy

### Stage 1 — specification

Complete this matrix from exact Toshiba remote/operation/service manuals.

No public behaviour change required.

### Stage 2 — targeted UART validation

Populate register mapping, side effects and old/new firmware results from the four installed units.

### Stage 3 — independent entities

Add separate ESPHome entities while preserving `supported_presets` temporarily for compatibility.

### Stage 4 — deprecate false preset abstraction

Mark `special_mode` / `supported_presets` legacy where they represent independent Toshiba controls.

### Stage 5 — breaking cleanup

Remove obsolete preset mapping in a clearly identified breaking release after the independent controls have been proven on the target model families.

---

# 11. Immediate implementation findings from current repository

The current component already contains several clues supporting this redesign:

- `power_select` is already an independent ESPHome select and should remain so.
- fixed vertical air direction is already an independent select and should remain logically separate from swing.
- `supported_presets` currently groups Standard, Hi POWER, ECO, Fireplace, 8 degrees, Silent, Sleep, Floor and Comfort into one public preset dimension.
- `SPECIAL_MODE` maps those values through register 0xF7.
- `COMFORT_SLEEP` also exists independently at command/register 0x94, indicating that the public `SPECIAL_MODE` abstraction should not be assumed to represent Toshiba's full state model.
- the fan implementation already uses custom fan levels; these should be renamed to Toshiba's documented LOW+ / MED+ terminology rather than Low-Medium / Medium-High.

These findings are enough to justify the architectural change, but **not** enough to claim full protocol support for each independent feature. The UART validation programme above remains required.

---

## Acceptance criterion

The redesign is complete only when:

1. the Home Assistant UI divisions match Toshiba's own remote/panel divisions;
2. valid combinations available from the Toshiba control can also be represented in Home Assistant;
3. invalid combinations resolve in the same way as the physical Toshiba control;
4. side effects such as FLOOR forcing Fan Auto are reflected from actual Toshiba state rather than fabricated optimistically by ESPHome;
5. firmware-specific limitations are explicit rather than hidden behind model-family assumptions.
