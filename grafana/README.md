# Grafana Dashboards

This folder contains exported Grafana dashboard definitions.

## Import

1. In Grafana, go to **Dashboards → Import**
2. Upload the JSON file from the `dashboards/` folder
3. Select the correct data source when prompted (see below)

## Required Data Source

| Template variable | Type  | Description                        |
|-------------------|-------|------------------------------------|
| `DS_MYSQL`        | MySQL | Database containing the `sml_v1` and `clients` tables |

## Note on CO2 and electricity price panels

Some panels in **MesswerteImDetail** query a second database (`hz-v1`) for CO2 intensity and day-ahead electricity prices. These panels will show no data unless you provide the following tables in a database accessible via `DS_MYSQL`:

- `co2_intensity_hourly` — columns: `zone`, `emission_type`, `forecast_type`, `ts_utc`, `value`
- `power_prices` — columns: `bidding_zone`, `unix_timestamp`, `price_eur_mwh`

If you do not have this data, simply hide or delete the affected panels (CO2, electricity price, calculated costs) after import.

## Database Schema

See [`../backend/database_setup.md`](../backend/database_setup.md) for the full table definitions.
