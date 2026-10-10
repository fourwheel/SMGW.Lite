#pragma once

void handle_wifi_setup_lifecycle();
void handle_check_wifi_connection();
void handle_backend_recovery();
void supervisorTask(void *pvParameters);
