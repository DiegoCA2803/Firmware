#include <WiFi.h>
#include <ESP32Servo.h>

//  WIFI 
const char* ssid = "";
const char* password = "";

//  PINES =
const int pinMQ6 = 34;          // lectura MQ6
const int pinBuzzer = 25;       
const int pinServo = 27;        
const int pinVentilador = 14;   //transistor que controla relé/ventilador
const int pinBoton = 33;        

//  UMBRALES 
const int umbralAlerta = 1500;  // Detecta gas desde este valor
const int umbralSeguro = 1300;  // Permite abrir con botón cuando baja a este valor

//  SERVO 
Servo servoValvula;

const int servoAbierto = 0;
const int servoCerrado = 90;

//  ESTADO 
bool valvulaCerradaPorGas = false;

// Antirrebote botón
unsigned long ultimoTiempoBoton = 0;
const unsigned long debounceMs = 300;

// FUNCIONES 
void activarBuzzer() {
  digitalWrite(pinBuzzer, HIGH);
}

void apagarBuzzer() {
  digitalWrite(pinBuzzer, LOW);
}

void encenderVentilador() {
  digitalWrite(pinVentilador, HIGH);
}

void apagarVentilador() {
  digitalWrite(pinVentilador, LOW);
}

void cerrarValvula() {
  servoValvula.write(servoCerrado);
}

void abrirValvula() {
  servoValvula.write(servoAbierto);
}

//  SETUP 
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

  Serial.println("Sistema SIMOGAS iniciando...");
  Serial.println("Servo ABIERTO");
  Serial.println("Ventilador APAGADO");
  Serial.println("Buzzer APAGADO");

  Serial.print("Conectando a WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi conectado correctamente");
    Serial.print("IP asignada: ");
    Serial.println(WiFi.localIP());

    activarBuzzer();
    delay(200);
    apagarBuzzer();
  } else {
    Serial.println("No se pudo conectar al WiFi");

    for (int i = 0; i < 3; i++) {
      activarBuzzer();
      delay(200);
      apagarBuzzer();
      delay(200);
    }
  }

  Serial.println("Iniciando lectura del MQ6...");
}

// LOOP 
void loop() {
  int valorMQ6 = analogRead(pinMQ6);
  float voltajeD34 = (valorMQ6 * 3.3) / 4095.0;

  bool gasDetectado = valorMQ6 >= umbralAlerta;
  bool ambienteSeguro = valorMQ6 <= umbralSeguro;

  if (gasDetectado) {
    valvulaCerradaPorGas = true;

    cerrarValvula();
    activarBuzzer();
    encenderVentilador();
  } else {
    apagarBuzzer();
    apagarVentilador();

    // No abrir automáticamente la válvula.
    // Solo se abre con el botón si el ambiente ya está seguro.
    if (valvulaCerradaPorGas) {
      cerrarValvula();
    } else {
      abrirValvula();
    }
  }

  // Botón para reabrir válvula solo si ya no hay gas alto
  if (digitalRead(pinBoton) == LOW) {
    unsigned long ahora = millis();

    if (ahora - ultimoTiempoBoton > debounceMs) {
      ultimoTiempoBoton = ahora;

      Serial.println("Boton presionado");

      if (ambienteSeguro) {
        valvulaCerradaPorGas = false;
        abrirValvula();
        apagarBuzzer();
        apagarVentilador();

        Serial.println("Sistema reiniciado: valvula ABIERTA");
      } else {
        Serial.println("No se puede abrir la valvula: lectura de gas aun alta");
      }
    }
  }

  Serial.println("-----------------------------");
  Serial.print("Valor MQ6 ADC: ");
  Serial.println(valorMQ6);

  Serial.print("Voltaje aproximado en D34: ");
  Serial.print(voltajeD34);
  Serial.println(" V");

  Serial.print("WiFi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "conectado" : "desconectado");

  Serial.print("Gas detectado: ");
  Serial.println(gasDetectado ? "SI" : "NO");

  Serial.print("Ambiente seguro para boton: ");
  Serial.println(ambienteSeguro ? "SI" : "NO");

  Serial.print("Valvula: ");
  Serial.println(valvulaCerradaPorGas ? "CERRADA" : "ABIERTA");

  Serial.print("Ventilador: ");
  Serial.println(gasDetectado ? "ENCENDIDO" : "APAGADO");

  delay(500);
}