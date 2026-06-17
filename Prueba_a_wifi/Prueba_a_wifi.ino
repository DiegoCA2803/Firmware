#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <time.h>

const char* ssid = "";
const char* password = "";

const char* mqttHost = "";
const int mqttPort = ;
const char* mqttUser = "";
const char* mqttPassword = "";

const char* sensorId = "esp32-001";
const char* firmwareVersion = "1.1.0";

const int pinMQ6 = 34;
const int pinBuzzer = 25;
const int pinServo = 27;
const int pinVentilador = 14;
const int pinBoton = 33;

const int umbralAlerta = 1500;
const int umbralSeguro = 1300;

const int servoAbierto = 90;
const int servoCerrado = 0;

const int ventiladorEncendido = HIGH;
const int ventiladorApagado = LOW;

const unsigned long intervaloLecturaMs = 500;
const unsigned long intervaloGuardadoMs = 1000;
const unsigned long intervaloReconexionWifiMs = 8000;
const unsigned long intervaloReconexionMqttMs = 5000;
const unsigned long debounceMs = 300;

const int maxRegistrosOffline = 180;
const int registrosPorSubida = 5;

Servo servoValvula;
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

bool valvulaCerradaPorGas = false;
bool ventiladorActivo = false;
bool ventiladorEncendidoPorBackend = false;
bool ventiladorEncendidoPorGas = false;
unsigned long tiempoGasDesaparecido = 0;
bool buzzerActivo = false;
bool wifiIniciado = false;

int valorMQ6 = 0;
float voltajeD34 = 0.0;

unsigned long ultimaLectura = 0;
unsigned long ultimoGuardado = 0;
unsigned long ultimoIntentoWifi = 0;
unsigned long ultimoIntentoMqtt = 0;
unsigned long ultimoTiempoBoton = 0;

struct RegistroOffline {
  int gas;
  float voltaje;
  bool valvulaAbierta;
  bool ventiladorActivo;
  bool buzzerActivo;
  unsigned long uptimeSegundos;
  char timestamp[25];
};

RegistroOffline colaOffline[maxRegistrosOffline];
int inicioCola = 0;
int cantidadCola = 0;

// Devuelve el topic MQTT donde se publican las lecturas del sensor.
String topicData() {
  return "sensors/" + String(sensorId) + "/data";
}

// Devuelve el topic MQTT donde se publica el estado general del dispositivo.
String topicStatus() {
  return "sensors/" + String(sensorId) + "/status";
}

// Devuelve el topic MQTT donde el ESP32 escucha comandos remotos.
String topicCommand() {
  return "sensors/" + String(sensorId) + "/command";
}

// Devuelve el topic MQTT donde el ESP32 escucha comandos de valvula.
String topicValveCommand() {
  return "gas/command/" + String(sensorId) + "/valve";
}

// Devuelve el topic MQTT donde el ESP32 escucha comandos de disipador.
String topicDissipatorCommand() {
  return "gas/command/" + String(sensorId) + "/dissipator";
}

// Devuelve el topic MQTT donde el ESP32 podría recibir configuración remota.
String topicConfig() {
  return "sensors/" + String(sensorId) + "/config";
}

// Enciende el buzzer y actualiza su estado interno.
void activarBuzzer() {
  digitalWrite(pinBuzzer, HIGH);
  buzzerActivo = true;
}

// Apaga el buzzer y actualiza su estado interno.
void apagarBuzzer() {
  digitalWrite(pinBuzzer, LOW);
  buzzerActivo = false;
}

// Enciende el ventilador o disipador y actualiza su estado interno.
void encenderVentilador() {
  digitalWrite(pinVentilador, ventiladorEncendido);
  ventiladorActivo = true;
}

// Apaga el ventilador o disipador y actualiza su estado interno.
void apagarVentilador() {
  digitalWrite(pinVentilador, ventiladorApagado);
  ventiladorActivo = false;
}

// Mueve el servomotor a la posición de válvula cerrada.
void cerrarValvula() {
  servoValvula.write(servoCerrado);
}

// Mueve el servomotor a la posición de válvula abierta.
void abrirValvula() {
  servoValvula.write(servoAbierto);
}

// Indica si la lectura actual de gas ya está por debajo del umbral seguro.
bool ambienteSeguro() {
  return valorMQ6 <= umbralSeguro;
}

// Indica si la lectura actual supera el umbral de alerta de gas.
bool gasDetectado() {
  return valorMQ6 >= umbralAlerta;
}

// Verifica si el ESP32 ya tiene una hora válida sincronizada por internet.
bool horaValida() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 100)) {
    return false;
  }

  return timeinfo.tm_year + 1900 >= 2024;
}

// Obtiene la fecha y hora actual en formato ISO8601 para enviarla al backend.
String obtenerTimestamp() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 100)) {
    return "1970-01-01T00:00:00Z";
  }

  if (timeinfo.tm_year + 1900 < 2024) {
    return "1970-01-01T00:00:00Z";
  }

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

// Lee el valor analógico del sensor MQ6 y calcula el voltaje aproximado del pin.
void leerSensor() {
  valorMQ6 = analogRead(pinMQ6);
  voltajeD34 = (valorMQ6 * 3.3) / 4095.0;
}

// Ejecuta la lógica de seguridad local sin depender del backend ni de internet.
void aplicarLogicaLocal() {
  if (gasDetectado()) {
    valvulaCerradaPorGas = true;
    cerrarValvula();
    activarBuzzer();
    
    // Encender ventilador debido a detección de gas
    ventiladorEncendidoPorGas = true;
    tiempoGasDesaparecido = 0; // Resetear temporizador de cooldown
    encenderVentilador();
    return;
  }

  apagarBuzzer();

  // Lógica de apagado diferido (cooldown de 5 segundos)
  if (ventiladorEncendidoPorGas) {
    if (tiempoGasDesaparecido == 0) {
      tiempoGasDesaparecido = millis(); // Registrar cuándo desapareció el gas
    }
    
    if (millis() - tiempoGasDesaparecido >= 5000) {
      ventiladorEncendidoPorGas = false;
      tiempoGasDesaparecido = 0;
      if (!ventiladorEncendidoPorBackend) {
        apagarVentilador();
      }
    }
  } else {
    if (!ventiladorEncendidoPorBackend) {
      apagarVentilador();
    }
  }

  if (valvulaCerradaPorGas) {
    cerrarValvula();
  } else {
    abrirValvula();
  }
}

// Revisa el botón físico para reabrir la válvula solo cuando el ambiente ya es seguro.
void revisarBoton() {
  if (digitalRead(pinBoton) == LOW) {
    unsigned long ahora = millis();

    if (ahora - ultimoTiempoBoton > debounceMs) {
      ultimoTiempoBoton = ahora;

      Serial.println("Boton presionado");

      if (ambienteSeguro()) {
        valvulaCerradaPorGas = false;
        abrirValvula();
        apagarBuzzer();
        
        // Resetear estados del ventilador
        ventiladorEncendidoPorBackend = false;
        ventiladorEncendidoPorGas = false;
        tiempoGasDesaparecido = 0;
        apagarVentilador();

        Serial.println("Sistema reiniciado: valvula ABIERTA");
      } else {
        Serial.println("No se puede abrir la valvula: lectura de gas aun alta");
      }
    }
  }
}

// Inicia la conexión WiFi una sola vez para evitar reinicios constantes de conexión.
void iniciarWifi() {
  if (wifiIniciado) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  wifiIniciado = true;

  Serial.print("Conectando a WiFi: ");
  Serial.println(ssid);
}

// Mantiene la conexión WiFi activa y reintenta conectarse si se pierde la señal.
void mantenerWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  unsigned long ahora = millis();

  if (ahora - ultimoIntentoWifi >= intervaloReconexionWifiMs) {
    ultimoIntentoWifi = ahora;

    Serial.println("Reintentando WiFi...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }
}

// Publica el estado general del dispositivo por MQTT.
void publicarEstado(const char* estado) {
  if (!mqttClient.connected()) {
    return;
  }

  StaticJsonDocument<512> doc;

  doc["sensor_id"] = sensorId;
  doc["status"] = estado;
  doc["timestamp"] = obtenerTimestamp();
  doc["ip_address"] = WiFi.localIP().toString();

  JsonObject deviceStatus = doc.createNestedObject("device_status");
  deviceStatus["valve_open"] = !valvulaCerradaPorGas;
  deviceStatus["dissipator_active"] = ventiladorActivo;
  deviceStatus["buzzer_active"] = buzzerActivo;
  deviceStatus["gas_raw"] = valorMQ6;

  JsonObject metadata = doc.createNestedObject("metadata");
  metadata["firmware_version"] = firmwareVersion;
  metadata["wifi_rssi"] = WiFi.RSSI();
  metadata["uptime_seconds"] = millis() / 1000;
  metadata["offline_queue_count"] = cantidadCola;

  char buffer[512];
  serializeJson(doc, buffer);

  mqttClient.publish(topicStatus().c_str(), buffer, true);
}

// Publica una lectura del sensor por MQTT y marca si viene de la cola offline.
bool publicarRegistro(const RegistroOffline& registro, bool vieneDeCola) {
  if (!mqttClient.connected()) {
    return false;
  }

  StaticJsonDocument<768> doc;

  doc["sensor_id"] = sensorId;

  if (strlen(registro.timestamp) > 0) {
    doc["timestamp"] = registro.timestamp;
  } else {
    doc["timestamp"] = obtenerTimestamp();
  }

  JsonObject readings = doc.createNestedObject("readings");
  readings["gas_concentration"] = registro.gas;
  readings["temperature"] = 0.0;
  readings["humidity"] = 0.0;

  JsonObject status = doc.createNestedObject("status");
  status["valve_open"] = registro.valvulaAbierta;
  status["dissipator_active"] = registro.ventiladorActivo;
  status["battery_level"] = 100.0;

  JsonObject metadata = doc.createNestedObject("metadata");
  metadata["firmware_version"] = firmwareVersion;
  metadata["wifi_rssi"] = WiFi.RSSI();
  metadata["uptime_seconds"] = millis() / 1000;
  metadata["captured_uptime_seconds"] = registro.uptimeSegundos;
  metadata["gas_raw_adc"] = registro.gas;
  metadata["voltage_d34"] = registro.voltaje;
  metadata["threshold_alert"] = umbralAlerta;
  metadata["threshold_safe"] = umbralSeguro;
  metadata["offline_record"] = vieneDeCola;

  char buffer[768];
  serializeJson(doc, buffer);

  return mqttClient.publish(topicData().c_str(), buffer);
}

// Guarda una lectura en memoria cuando no hay conexión MQTT disponible.
void guardarRegistroOffline(const RegistroOffline& registro) {
  int posicion;

  if (cantidadCola < maxRegistrosOffline) {
    posicion = (inicioCola + cantidadCola) % maxRegistrosOffline;
    cantidadCola++;
  } else {
    posicion = inicioCola;
    inicioCola = (inicioCola + 1) % maxRegistrosOffline;
  }

  colaOffline[posicion] = registro;

  Serial.print("Registro guardado offline. Pendientes: ");
  Serial.println(cantidadCola);
}

// Crea un registro con la lectura actual y el estado actual del dispositivo.
RegistroOffline crearRegistroActual() {
  RegistroOffline registro;

  registro.gas = valorMQ6;
  registro.voltaje = voltajeD34;
  registro.valvulaAbierta = !valvulaCerradaPorGas;
  registro.ventiladorActivo = ventiladorActivo;
  registro.buzzerActivo = buzzerActivo;
  registro.uptimeSegundos = millis() / 1000;
  registro.timestamp[0] = '\0';

  if (horaValida()) {
    String ts = obtenerTimestamp();
    ts.toCharArray(registro.timestamp, sizeof(registro.timestamp));
  }

  return registro;
}

// Publica la lectura actual si hay conexión; si falla, la guarda en la cola offline.
void guardarOPublicarLectura() {
  RegistroOffline registro = crearRegistroActual();

  if (mqttClient.connected()) {
    bool publicado = publicarRegistro(registro, false);

    if (publicado) {
      Serial.println("Lectura publicada por MQTT");
    } else {
      guardarRegistroOffline(registro);
      Serial.println("No se pudo publicar. Guardado offline");
    }
  } else {
    guardarRegistroOffline(registro);
  }
}

// Sube al backend los registros guardados mientras el ESP32 estuvo sin conexión.
void subirPendientesOffline() {
  if (!mqttClient.connected()) {
    return;
  }

  int enviados = 0;

  while (cantidadCola > 0 && enviados < registrosPorSubida) {
    RegistroOffline registro = colaOffline[inicioCola];

    bool publicado = publicarRegistro(registro, true);

    if (!publicado) {
      Serial.println("Fallo subiendo registro offline. Se intentara luego");
      return;
    }

    inicioCola = (inicioCola + 1) % maxRegistrosOffline;
    cantidadCola--;
    enviados++;

    delay(20);
  }

  if (enviados > 0) {
    Serial.print("Registros offline subidos: ");
    Serial.println(enviados);

    Serial.print("Registros pendientes: ");
    Serial.println(cantidadCola);
  }
}

// Ejecuta los comandos remotos recibidos por MQTT, como cerrar válvula o activar ventilador.
void ejecutarComando(String command, String action) {
  command.toLowerCase();
  action.toLowerCase();

  if (command == "panic") {
    valvulaCerradaPorGas = true;
    cerrarValvula();
    activarBuzzer();
    ventiladorEncendidoPorBackend = true;
    encenderVentilador();
    Serial.println("Comando panic: valvula cerrada y ventilador encendido");
    return;
  }

  if (command == "valve_control" && action == "close") {
    valvulaCerradaPorGas = true;
    cerrarValvula();
    activarBuzzer();
    ventiladorEncendidoPorBackend = true;
    encenderVentilador();
    Serial.println("Comando MQTT: valvula cerrada");
    return;
  }

  if (command == "valve_control" && action == "open") {
    if (ambienteSeguro()) {
      valvulaCerradaPorGas = false;
      abrirValvula();
      apagarBuzzer();
      
      // Al reabrir de forma segura, apagamos ventilador si no hay alarma de gas activa
      ventiladorEncendidoPorBackend = false;
      ventiladorEncendidoPorGas = false;
      tiempoGasDesaparecido = 0;
      apagarVentilador();
      Serial.println("Comando MQTT: valvula abierta");
    } else {
      Serial.println("No se abre la valvula: lectura de gas aun alta");
    }

    return;
  }

  if (command == "dissipator_control" && action == "activate") {
    ventiladorEncendidoPorBackend = true;
    encenderVentilador();
    Serial.println("Comando MQTT: ventilador encendido");
    return;
  }

  if (command == "dissipator_control" && action == "deactivate") {
    ventiladorEncendidoPorBackend = false;
    if (ambienteSeguro()) {
      ventiladorEncendidoPorGas = false;
      tiempoGasDesaparecido = 0;
      apagarVentilador();
      Serial.println("Comando MQTT: ventilador apagado");
    } else {
      Serial.println("No se apaga el ventilador: gas aun alto");
    }

    return;
  }

  if (command == "on") {
    ventiladorEncendidoPorBackend = true;
    encenderVentilador();
    Serial.println("Comando compatible: ventilador encendido");
    return;
  }

  if (command == "off") {
    ventiladorEncendidoPorBackend = false;
    if (ambienteSeguro()) {
      ventiladorEncendidoPorGas = false;
      tiempoGasDesaparecido = 0;
      apagarVentilador();
      Serial.println("Comando compatible: ventilador apagado");
    } else {
      Serial.println("No se apaga el ventilador: gas aun alto");
    }

    return;
  }

  Serial.println("Comando MQTT no reconocido");
}

// Recibe mensajes MQTT, interpreta el JSON y envía el comando a la función correspondiente.
void recibirMqtt(char* topic, byte* payload, unsigned int length) {
  Serial.print("Mensaje recibido en topic: ");
  Serial.println(topic);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("Error JSON: ");
    Serial.println(error.c_str());
    return;
  }

  String topicStr = String(topic);
  String command = "";
  String action = "";

  if (topicStr.endsWith("/valve")) {
    command = "valve_control";
    action = doc["command"] | doc["action"] | "";
  } else if (topicStr.endsWith("/dissipator")) {
    command = "dissipator_control";
    String state = doc["state"] | doc["command"] | doc["action"] | "";
    state.toLowerCase();
    if (state == "on" || state == "activate" || state == "active") {
      action = "activate";
    } else if (state == "off" || state == "deactivate" || state == "inactive") {
      action = "deactivate";
    } else {
      action = state;
    }
  } else {
    // Canal de comandos antiguo o genérico
    command = doc["command"] | doc["state"] | "";
    action = doc["action"] | "";
  }

  ejecutarComando(command, action);
}

// Conecta el ESP32 al broker MQTT seguro y se suscribe a los topics de comando y configuración.
void conectarMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (mqttClient.connected()) {
    return;
  }

  Serial.print("Conectando a MQTT: ");
  Serial.println(mqttHost);

  String clientId = "SIMOGAS-" + String(sensorId);

  bool conectado = mqttClient.connect(clientId.c_str(), mqttUser, mqttPassword);

  if (conectado) {
    Serial.println("MQTT conectado correctamente");

    mqttClient.subscribe(topicCommand().c_str());
    mqttClient.subscribe(topicValveCommand().c_str());
    mqttClient.subscribe(topicDissipatorCommand().c_str());
    mqttClient.subscribe(topicConfig().c_str());

    Serial.println("Suscrito a los canales de comandos y configuracion:");
    Serial.print(" - "); Serial.println(topicCommand());
    Serial.print(" - "); Serial.println(topicValveCommand());
    Serial.print(" - "); Serial.println(topicDissipatorCommand());
    Serial.print(" - "); Serial.println(topicConfig());

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    publicarEstado("online");
  } else {
    Serial.print("Error MQTT, estado: ");
    Serial.println(mqttClient.state());
  }
}

// Mantiene viva la conexión MQTT y reintenta conectarse si se pierde.
void mantenerMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  unsigned long ahora = millis();

  if (ahora - ultimoIntentoMqtt >= intervaloReconexionMqttMs) {
    ultimoIntentoMqtt = ahora;
    conectarMqtt();
  }
}

// Imprime en el monitor serial el estado actual del sensor, conexión y actuadores.
void imprimirEstado() {
  Serial.println("-----------------------------");
  Serial.print("Valor MQ6 ADC: ");
  Serial.println(valorMQ6);

  Serial.print("Voltaje aproximado en D34: ");
  Serial.print(voltajeD34);
  Serial.println(" V");

  Serial.print("WiFi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "conectado" : "desconectado");

  Serial.print("MQTT: ");
  Serial.println(mqttClient.connected() ? "conectado" : "desconectado");

  Serial.print("Gas detectado: ");
  Serial.println(gasDetectado() ? "SI" : "NO");

  Serial.print("Ambiente seguro para boton: ");
  Serial.println(ambienteSeguro() ? "SI" : "NO");

  Serial.print("Valvula: ");
  Serial.println(valvulaCerradaPorGas ? "CERRADA" : "ABIERTA");

  Serial.print("Ventilador: ");
  Serial.println(ventiladorActivo ? "ENCENDIDO" : "APAGADO");

  Serial.print("Registros offline pendientes: ");
  Serial.println(cantidadCola);
}

// Configura pines, servo, MQTT, WiFi y deja el sistema listo para iniciar lecturas.
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(pinBuzzer, OUTPUT);
  pinMode(pinVentilador, OUTPUT);
  pinMode(pinBoton, INPUT_PULLUP);

  apagarBuzzer();
  apagarVentilador();

  servoValvula.setPeriodHertz(50);
  servoValvula.attach(pinServo, 500, 2400);
  abrirValvula();

  wifiClient.setInsecure();

  mqttClient.setServer(mqttHost, mqttPort);
  mqttClient.setCallback(recibirMqtt);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(30);
  mqttClient.setBufferSize(1024);

  Serial.println("Sistema SIMOGAS iniciando...");
  Serial.println("Sensor ID: esp32-001");
  Serial.println("Modo autonomo activado");
  Serial.println("Servo ABIERTO");
  Serial.println("Ventilador APAGADO");
  Serial.println("Buzzer APAGADO");

  iniciarWifi();

  Serial.println("Iniciando lectura del MQ6...");
}

// Ciclo principal: mantiene conexiones, lee gas, aplica seguridad local, publica datos y sube pendientes.
void loop() {
  mantenerWifi();
  mantenerMqtt();
  revisarBoton();

  unsigned long ahora = millis();

  if (ahora - ultimaLectura >= intervaloLecturaMs) {
    ultimaLectura = ahora;

    leerSensor();
    aplicarLogicaLocal();
    imprimirEstado();
  }

  if (ahora - ultimoGuardado >= intervaloGuardadoMs) {
    ultimoGuardado = ahora;

    guardarOPublicarLectura();
    publicarEstado("online");
  }

  subirPendientesOffline();
}