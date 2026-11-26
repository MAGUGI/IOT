// ============================================================
// Alimentador de cachorro - parte do ARDUINO UNO
//
// O Uno cuida do "corpo" do alimentador:
//   - lê o sensor ultrassônico HC-SR04
//   - move o servo que abre a tampa da garrafa
// e conversa com o ESP32 (que cuida do Wi-Fi e do MQTT) por uma
// porta serial nos pinos D2 e D3.
//
// Mensagens trocadas (texto, uma por linha):
//   ESP32 -> Uno:  ABRIR          pede para soltar ração
//   Uno -> ESP32:  DIST:12.3      distância medida, em cm (a cada 2 s)
//                  ABRINDO        a tampa começou a abrir
//                  FECHADO        a tampa fechou de novo
//
// Bibliotecas: Servo e SoftwareSerial (já vêm com a Arduino IDE).
// ============================================================

#include <Servo.h>
#include <SoftwareSerial.h>

// ---------- Pinos ----------
const int PINO_SERVO = 13;
const int PINO_TRIG  = 9;
const int PINO_ECHO  = 10;
const int PINO_RX    = 2;   // recebe do ESP32 (fio vindo do GPIO 17)
const int PINO_TX    = 3;   // envia ao ESP32 (fio vai ao GPIO 16, pelo divisor)

// ---------- Servo ----------
const int ANGULO_FECHADO = 0;
const int ANGULO_ABERTO  = 90;
const unsigned long TEMPO_ABERTO_MS = 5000;   // tampa aberta por 5 s

// ---------- Sensor ----------
const unsigned long INTERVALO_LEITURA_MS = 2000;  // envia a distância a cada 2 s

SoftwareSerial esp(PINO_RX, PINO_TX);  // porta serial "de software" com o ESP32
Servo servo;

String linha = "";              // junta as letras até chegar o fim da linha
unsigned long ultimaLeitura = 0;

// Mede a distância uma vez, em cm (retorna -1 se não detectar nada)
float medirUmaVez() {
  digitalWrite(PINO_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PINO_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PINO_TRIG, LOW);

  long duracao = pulseIn(PINO_ECHO, HIGH, 30000);
  if (duracao == 0) {
    return -1;
  }
  return duracao * 0.0343 / 2;
}

// Tenta medir até 3 vezes e devolve a primeira leitura válida (ou -1)
float medirDistancia() {
  for (int tentativa = 0; tentativa < 3; tentativa++) {
    float d = medirUmaVez();
    if (d > 0) return d;
    delay(30);  // o sensor precisa de um intervalo entre medições
  }
  return -1;
}

// Mede e manda a distância ao ESP32 (ex.: "DIST:12.3")
void enviarDistancia() {
  float distancia = medirDistancia();
  if (distancia < 0) {
    Serial.println("Fora de alcance");
    return;  // sem leitura válida, não manda nada
  }
  esp.print("DIST:");
  esp.println(distancia, 1);

  Serial.print("Distancia: ");
  Serial.print(distancia);
  Serial.println(" cm (enviada ao ESP32)");
}

// Abre a tampa por 5 s e fecha, avisando o ESP32 antes e depois.
// O servo só recebe sinal (attach) durante o movimento. Parado, fica sem
// sinal (detach) e não treme quando o Uno envia mensagens pela SoftwareSerial.
void abrirTampa() {
  esp.println("ABRINDO");
  servo.attach(PINO_SERVO);
  Serial.println("Abrindo a tampa...");
  servo.write(ANGULO_ABERTO);
  delay(TEMPO_ABERTO_MS);

  servo.write(ANGULO_FECHADO);
  delay(500);  // tempo para o servo voltar
  servo.detach();
  esp.println("FECHADO");
  Serial.println("Tampa fechada.");

  enviarDistancia();  // nível atualizado logo depois de soltar a ração
}

// Lê as letras que chegaram do ESP32 e junta numa linha
void lerEsp() {
  while (esp.available()) {
    char c = esp.read();
    if (c == '\n') {           // fim da linha: a mensagem está completa
      linha.trim();            // tira o '\r' e espaços
      Serial.print("Recebido do ESP32: ");
      Serial.println(linha);
      if (linha == "ABRIR") {
        abrirTampa();
      }
      linha = "";
    } else {
      linha += c;
      if (linha.length() > 20) linha = "";  // lixo na linha: descarta
    }
  }
}

void setup() {
  Serial.begin(9600);   // Monitor Serial do Uno (USB)
  esp.begin(9600);      // conversa com o ESP32

  servo.attach(PINO_SERVO);   // vai para a posição fechada ao ligar...
  servo.write(ANGULO_FECHADO);
  delay(500);
  servo.detach();             // ...e fica sem sinal até o próximo comando

  pinMode(PINO_TRIG, OUTPUT);
  pinMode(PINO_ECHO, INPUT);

  Serial.println("Uno pronto.");
}

void loop() {
  lerEsp();

  if (millis() - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = millis();
    enviarDistancia();
  }
}
