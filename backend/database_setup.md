
CREATE TABLE `clients` (
  `id` int(10) UNSIGNED NOT NULL,
  `device_id` char(3) NOT NULL,
  `token` char(64) DEFAULT NULL,
  `grafana_ids` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL,
  `endpoint` varchar(100) DEFAULT NULL,
  `wireframe` varchar(30) DEFAULT NULL,
  `first_reading` datetime DEFAULT NULL,
  `last_reading` datetime DEFAULT NULL,
  `comment` text DEFAULT NULL,
  `pin_meter` tinyint(1) DEFAULT NULL COMMENT 'NULL = not applicable (tks); 0 = no PIN (whole kWh only); 1 = PIN unlocked (sub-kWh precision)',
  `fw_version` varchar(20) DEFAULT NULL COMMENT 'Firmware version string, e.g. 1.2.0',
  `cfg_version` varchar(10) DEFAULT NULL COMMENT 'IotWebConf config schema version, e.g. 2906'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

-- Migration for existing installations:
-- ALTER TABLE `clients` ADD COLUMN `pin_meter` TINYINT(1) DEFAULT NULL COMMENT 'NULL = not applicable (tks); 0 = no PIN (whole kWh only); 1 = PIN unlocked (sub-kWh precision)';
-- ALTER TABLE `clients` ADD COLUMN `fw_version` VARCHAR(20) DEFAULT NULL COMMENT 'Firmware version string, e.g. 1.2.0';
-- ALTER TABLE `clients` ADD COLUMN `cfg_version` VARCHAR(10) DEFAULT NULL COMMENT 'IotWebConf config schema version, e.g. 2906';

-- --------------------------------------------------------

--
-- Table structure for table `sml_v1`
--

CREATE TABLE `sml_v1` (
  `i` int(11) NOT NULL,
  `id` varchar(3) NOT NULL,
  `timestamp_server2` int(10) UNSIGNED NOT NULL,
  `timestamp_client` int(10) UNSIGNED DEFAULT NULL,
  `meter_value` int(10) UNSIGNED DEFAULT NULL COMMENT '0.1 Wh',
  `meter_value_PV` bigint(20) DEFAULT NULL,
  `temperature` float DEFAULT NULL,
  `obis280` int(10) UNSIGNED DEFAULT NULL COMMENT '0.1 Wh'
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

--
-- Indexes for exported tables
--

--
-- Indexes for table `clients`
--
ALTER TABLE `clients`
  ADD PRIMARY KEY (`id`),
  ADD UNIQUE KEY `uq_device_id` (`device_id`);

--
-- Indexes for table `sml_v1`
--
ALTER TABLE `sml_v1`
  ADD PRIMARY KEY (`i`),
  ADD UNIQUE KEY `id` (`i`),
  ADD KEY `idx_sml_v1_timestamp_client` (`timestamp_client`),
  ADD KEY `id_2` (`id`),
  ADD KEY `idx_id_timestamp` (`id`,`timestamp_server2`);

--
-- AUTO_INCREMENT for exported tables
--

--
-- AUTO_INCREMENT for table `clients`
--
ALTER TABLE `clients`
  MODIFY `id` int(10) UNSIGNED NOT NULL AUTO_INCREMENT;

--
-- AUTO_INCREMENT for table `sml_v1`
--
ALTER TABLE `sml_v1`
  MODIFY `i` int(11) NOT NULL AUTO_INCREMENT;
COMMIT;

-- --------------------------------------------------------

--
-- Table structure for table `device_logs`
-- Stores the binary log buffer sent by each ESP32 device.
-- Entries older than 30 days are deleted automatically by log.php on each POST.
--

CREATE TABLE `device_logs` (
  `id`               int(10) UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`        varchar(3)       NOT NULL,
  `timestamp_client` int(10) UNSIGNED NOT NULL COMMENT 'Unix epoch from the device clock',
  `uptime_ms`        int(10) UNSIGNED NOT NULL COMMENT 'millis() at the time of the log entry',
  `status_code`      int(11)          NOT NULL,
  `received_at`      datetime         NOT NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`id`),
  -- Prevents duplicate rows when the device re-sends the same buffer
  UNIQUE KEY `uq_log_entry` (`device_id`, `timestamp_client`, `uptime_ms`, `status_code`),
  KEY `idx_device_received` (`device_id`, `received_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

--
-- Triggers for table `clients`
-- Automatically hashes the token with SHA-256 on insert and update,
-- so plain-text tokens can be entered directly (e.g. via phpMyAdmin).
--
DELIMITER //

CREATE TRIGGER hash_token_before_insert
BEFORE INSERT ON clients
FOR EACH ROW
BEGIN
    IF NEW.token IS NOT NULL THEN
        SET NEW.token = SHA2(NEW.token, 256);
    END IF;
END//

CREATE TRIGGER hash_token_before_update
BEFORE UPDATE ON clients
FOR EACH ROW
BEGIN
    IF NEW.token IS NOT NULL AND NEW.token != OLD.token THEN
        SET NEW.token = SHA2(NEW.token, 256);
    END IF;
END//

DELIMITER ;

-- --------------------------------------------------------

--
-- Table structure for table `sml_diag`
-- Stores per-entry diagnostic data for every batch transmission.
-- Both accepted (is_ok=1) and rejected (is_ok=0) rows are written.
-- Entries older than 30 days are deleted automatically by index.php on each POST.
--

CREATE TABLE `sml_diag` (
  `id`               BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT,
  `device_id`        VARCHAR(64)       NOT NULL,
  `received_at`      DATETIME          NOT NULL DEFAULT current_timestamp(),
  `batch_received`   SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `batch_inserted`   SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `batch_rejected`   SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `entry_index`      SMALLINT UNSIGNED NOT NULL,
  `timestamp_client` INT UNSIGNED      NOT NULL,
  `meter`            BIGINT UNSIGNED   NOT NULL,
  `solar`            DOUBLE            NULL,
  `temp_c`           DOUBLE            NULL,
  `power_w`          INT               NOT NULL DEFAULT 0,
  `prev_ts`          INT UNSIGNED      NOT NULL DEFAULT 0,
  `prev_meter`       BIGINT UNSIGNED   NOT NULL DEFAULT 0,
  `status`           VARCHAR(64)       NOT NULL DEFAULT 'OK',
  `is_ok`            TINYINT(1)        NOT NULL DEFAULT 1,
  PRIMARY KEY (`id`),
  KEY `idx_device_received` (`device_id`, `received_at`),
  KEY `idx_received_at`     (`received_at`),
  KEY `idx_is_ok`           (`is_ok`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
