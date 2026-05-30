
CREATE TABLE `clients` (
  `id` int(10) UNSIGNED NOT NULL,
  `device_id` char(3) NOT NULL,
  `token` char(64) DEFAULT NULL,
  `grafana_ids` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL,
  `endpoint` varchar(100) DEFAULT NULL,
  `wireframe` varchar(30) DEFAULT NULL,
  `first_reading` datetime DEFAULT NULL,
  `last_reading` datetime DEFAULT NULL,
  `comment` text DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

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
