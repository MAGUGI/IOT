// ============================================================
// Alimentador de cachorro - parte do ESP32 (Wi-Fi + MQTT)
// SEM bibliotecas externas: o MQTT é feito "à mão" sobre uma
// conexão TCP (WiFiClient).
//
// O ESP32 não tem sensor nem servo ligados nele: quem cuida deles é
// o Arduino Uno (firmware/uno_atuador). Os dois conversam por serial:
//   ESP32 GPIO 17 (TX2) -> Uno D2 (RX)
//   Uno D3 (TX) -> divisor de tensão -> ESP32 GPIO 16 (RX2)
//
// Tópicos (TOPICO_BASE é declarado logo abaixo, na configuração):
//   <base>/distancia ESP32 -> dashboard distância em cm (ex.: "16.3")
//   <base>/nivel    ESP32 -> dashboard  "Cheio" ou "Vazio"
//   <base>/estado   ESP32 -> dashboard  "Fechado" ou "Liberando ração"
//   <base>/status   ESP32 -> dashboard  "online" ou "offline" (Last Will)
//   <base>/comando  dashboard -> ESP32  "liberar"
// ============================================================

#include <WiFi.h>

// ---------- Configuração ----------
// Preencham o Wi-Fi para gravar. NÃO subam a senha real para o GitHub:
// no repositório, deixem os valores de exemplo abaixo.
const char* WIFI_SSID   = "NOME_DA_SUA_REDE";   // rede de 2,4 GHz
const char* WIFI_SENHA  = "SENHA_DA_SUA_REDE";

const char* MQTT_BROKER = "broker.hivemq.com";  // broker público da HiveMQ
const int   MQTT_PORTA  = 1883;

// Começo de todos os tópicos. IGUAL ao TOPICO_BASE do index.html.
const char* TOPICO_BASE = "catolica/iot/alimentador";

// ---------- Pinos da serial com o Uno ----------
const int PINO_RX2 = 16;  // recebe do Uno (D3), pelo divisor de tensão
const int PINO_TX2 = 17;  // envia ao Uno (D2)

// ---------- Reservatório ----------
// Se a ração estiver mais longe do sensor que isto, a garrafa está "Vazio".
// Medir no Monitor Serial com pouca ração na garrafa e ajustar.
const float LIMITE_VAZIO_CM = 15.0;

// ---------- Tempos ----------
const unsigned long INTERVALO_RECONEXAO_WIFI = 10000;  // tenta o Wi-Fi a cada 10 s
const unsigned long INTERVALO_RECONEXAO_MQTT = 5000;   // tenta o broker a cada 5 s
const uint16_t KEEPALIVE_S = 30;  // "estou vivo" combinado com o broker

// ---------- Tópicos ----------
String topicoDistancia = String(TOPICO_BASE) + "/distancia";
String topicoNivel   = String(TOPICO_BASE) + "/nivel";
String topicoEstado  = String(TOPICO_BASE) + "/estado";
String topicoStatus  = String(TOPICO_BASE) + "/status";
String topicoComando = String(TOPICO_BASE) + "/comando";

WiFiClient rede;           // conexão TCP com o broker
bool mqttConectado = false;

unsigned long ultimaTentativaWifi = 0;
unsigned long ultimaTentativaMqtt = 0;
unsigned long ultimoPing = 0;       // último PINGREQ enviado ao broker
unsigned long ultimaRecepcao = 0;   // último pacote recebido do broker

void aoReceberMensagem(const String& topico, const String& mensagem);  // definida mais abaixo

// ============================================================
// MQTT feito à mão (protocolo MQTT 3.1.1)
// Todo pacote MQTT é: [tipo] [tamanho do resto] [resto]
// ============================================================

uint8_t pacote[256];  // espaço para montar um pacote antes de enviar
int pos;              // quantos bytes já foram colocados em "pacote"

// Coloca um texto no pacote no formato do MQTT: 2 bytes de tamanho + o texto
void adicionarTexto(const char* texto) {
  uint16_t n = strlen(texto);
  pacote[pos++] = n >> 8;     // byte alto do tamanho
  pacote[pos++] = n & 0xFF;   // byte baixo do tamanho
  memcpy(pacote + pos, texto, n);
  pos += n;
}

// Envia o pacote: primeiro o tipo, depois o tamanho do resto
// (o MQTT usa 7 bits por byte para o tamanho), depois o resto.
void enviarPacote(uint8_t tipo) {
  uint8_t cabecalho[5];
  int n = 0;
  cabecalho[n++] = tipo;
  int resto = pos;
  do {
    uint8_t b = resto % 128;
    resto = resto / 128;
    if (resto > 0) b = b | 0x80;  // bit 7 = "ainda tem mais um byte de tamanho"
    cabecalho[n++] = b;
  } while (resto > 0);

  rede.write(cabecalho, n);
  rede.write(pacote, pos);
}

// Espera chegar 1 byte (no máximo 3 s). Retorna -1 se não chegou.
int lerByte() {
  unsigned long inicio = millis();
  while (!rede.available()) {
    if (millis() - inicio > 3000 || !rede.connected()) return -1;
    delay(1);
  }
  return rede.read();
}

// CONNECT: apresenta o ESP32 ao broker e deixa a "Last Will"
bool mqttConectar() {
  if (!rede.connect(MQTT_BROKER, MQTT_PORTA)) return false;  // abre a conexão TCP

  String clientId = "alimentador-" + WiFi.macAddress();
  clientId.replace(":", "");

  pos = 0;
  adicionarTexto("MQTT");          // nome do protocolo
  pacote[pos++] = 4;               // versão 3.1.1
  // Flags: 0x02 sessão limpa | 0x04 tem Last Will | 0x08 Will QoS 1 | 0x20 Will com retain
  pacote[pos++] = 0x02 | 0x04 | 0x08 | 0x20;
  pacote[pos++] = KEEPALIVE_S >> 8;
  pacote[pos++] = KEEPALIVE_S & 0xFF;
  adicionarTexto(clientId.c_str());       // quem sou eu
  adicionarTexto(topicoStatus.c_str());   // Last Will: em qual tópico
  adicionarTexto("offline");              // Last Will: qual mensagem
  enviarPacote(0x10);                     // 0x10 = CONNECT

  // Resposta esperada (CONNACK): 0x20 0x02 0x00 0x00 (último 0x00 = aceito)
  int tipo = lerByte();
  int tam = lerByte();
  lerByte();               // flags da sessão (não usamos)
  int codigo = lerByte();
  if (tipo != 0x20 || tam != 2 || codigo != 0) {
    Serial.println("Broker nao aceitou a conexao.");
    rede.stop();
    return false;
  }
  ultimaRecepcao = millis();
  ultimoPing = millis();
  return true;
}

// PUBLISH: envia uma mensagem para um tópico (QoS 0)
void mqttPublicar(const String& topico, const char* mensagem, bool reter) {
  if (!mqttConectado) return;
  pos = 0;
  adicionarTexto(topico.c_str());
  int n = strlen(mensagem);
  memcpy(pacote + pos, mensagem, n);  // a mensagem vai sem os 2 bytes de tamanho
  pos += n;
  enviarPacote(reter ? 0x31 : 0x30);  // 0x30 = PUBLISH; +1 = retain
}

// SUBSCRIBE: pede ao broker para receber as mensagens de um tópico
void mqttAssinar(const String& topico) {
  pos = 0;
  pacote[pos++] = 0;  // id do pedido (2 bytes)
  pacote[pos++] = 1;
  adicionarTexto(topico.c_str());
  pacote[pos++] = 0;  // QoS 0
  enviarPacote(0x82); // 0x82 = SUBSCRIBE
}

// Lê os pacotes que chegaram do broker
void mqttReceber() {
  while (rede.available()) {
    int tipo = lerByte();
    if (tipo < 0) { rede.stop(); return; }

    // tamanho do resto (mesma regra de 7 bits do envio)
    int tamanho = 0, multiplicador = 1, b;
    do {
      b = lerByte();
      if (b < 0) { rede.stop(); return; }
      tamanho += (b & 0x7F) * multiplicador;
      multiplicador *= 128;
    } while (b & 0x80);

    // lê o resto do pacote
    int lidos = 0;
    while (lidos < tamanho) {
      int c = lerByte();
      if (c < 0) { rede.stop(); return; }
      if (lidos < (int)sizeof(pacote)) pacote[lidos] = c;
      lidos++;
    }
    ultimaRecepcao = millis();

    // Só interessa PUBLISH (tipo 0x3_). SUBACK e PINGRESP são ignorados.
    if ((tipo & 0xF0) == 0x30 && tamanho <= (int)sizeof(pacote)) {
      int tamTopico = (pacote[0] << 8) | pacote[1];
      String topico = "";
      for (int i = 0; i < tamTopico; i++) topico += (char)pacote[2 + i];
      int inicio = 2 + tamTopico;
      if (tipo & 0x06) inicio += 2;   // QoS 1 ou 2 trazem um id de 2 bytes
      String mensagem = "";
      for (int i = inicio; i < tamanho; i++) mensagem += (char)pacote[i];
      mensagem.trim();
      aoReceberMensagem(topico, mensagem);
    }
  }
}

// Mantém a conexão viva e percebe se o broker sumiu.
// A cada 15 s manda PINGREQ ("você está aí?"); o broker responde PINGRESP.
void mqttManterVivo() {
  if (millis() - ultimoPing > KEEPALIVE_S * 1000UL / 2) {
    ultimoPing = millis();
    pos = 0;
    enviarPacote(0xC0);  // 0xC0 = PINGREQ
  }
  // 30 s sem receber nada (nem a resposta do ping): a conexão morreu.
  if (millis() - ultimaRecepcao > KEEPALIVE_S * 1000UL) {
    Serial.println("Broker parou de responder.");
    rede.stop();
  }
}

// ============================================================
// Conversa com o Arduino Uno (porta Serial2 do ESP32)
// O Uno lê o sensor e move o servo; o ESP32 só repassa as mensagens.
//   ESP32 -> Uno:  ABRIR
//   Uno -> ESP32:  DIST:12.3   ABRINDO   FECHADO
// ============================================================

String linhaUno = "";         // junta as letras até chegar o fim da linha
const char* ultimoNivel = ""; // último nível calculado ("Cheio" ou "Vazio")

void publicarEstado(const char* estado) {
  Serial.printf("Estado: %s\n", estado);
  mqttPublicar(topicoEstado, estado, true);
}

// Trata uma mensagem completa que veio do Uno
void tratarMensagemUno(const String& msg) {
  if (msg.startsWith("DIST:")) {
    float distancia = msg.substring(5).toFloat();   // texto depois de "DIST:"
    // Ração perto do sensor = garrafa cheia; longe = vazia.
    ultimoNivel = (distancia > LIMITE_VAZIO_CM) ? "Vazio" : "Cheio";
    Serial.printf("Uno mediu %.1f cm | Nivel: %s\n", distancia, ultimoNivel);
    // retain = true: quem abrir o dashboard recebe o último valor na hora
    mqttPublicar(topicoDistancia, String(distancia, 1).c_str(), true);
    mqttPublicar(topicoNivel, ultimoNivel, true);
  } else if (msg == "ABRINDO") {
    publicarEstado("Liberando ração");
  } else if (msg == "FECHADO") {
    publicarEstado("Fechado");   // o estado vem do Uno, depois que o servo fechou
  }
}

// Lê as letras que chegaram do Uno, sem travar o loop
void lerUno() {
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c == '\n') {            // fim da linha: a mensagem está completa
      linhaUno.trim();          // tira o '\r' e espaços
      if (linhaUno.length() > 0) tratarMensagemUno(linhaUno);
      linhaUno = "";
    } else {
      linhaUno += c;
      if (linhaUno.length() > 30) linhaUno = "";  // lixo na linha: descarta
    }
  }
}

void aoReceberMensagem(const String& topico, const String& mensagem) {
  Serial.printf("Recebido [%s]: %s\n", topico.c_str(), mensagem.c_str());
  if (topico == topicoComando && mensagem == "liberar") {
    Serial2.println("ABRIR");   // repassa o pedido ao Uno
    Serial.println("Pedido ABRIR enviado ao Uno.");
  }
}

// ============================================================
// Reconexão automática (Wi-Fi e broker)
// Não usa while(): o loop continua rodando enquanto tenta.
// ============================================================

bool wifiEstavaConectado = false;

void verificarWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiEstavaConectado) {   // avisa só uma vez, quando conecta
      wifiEstavaConectado = true;
      Serial.printf("Wi-Fi conectado! IP: %s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }
  wifiEstavaConectado = false;
  if (millis() - ultimaTentativaWifi < INTERVALO_RECONEXAO_WIFI) return;
  ultimaTentativaWifi = millis();

  Serial.println("Wi-Fi desconectado. Tentando conectar...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
}

void verificarMqtt() {
  if (mqttConectado && !rede.connected()) {
    mqttConectado = false;
    Serial.println("Conexao com o broker caiu.");
  }
  if (WiFi.status() != WL_CONNECTED) return;  // sem Wi-Fi não adianta tentar
  if (mqttConectado) return;
  if (millis() - ultimaTentativaMqtt < INTERVALO_RECONEXAO_MQTT) return;
  ultimaTentativaMqtt = millis();

  Serial.print("Conectando ao broker... ");
  if (mqttConectar()) {
    mqttConectado = true;
    Serial.println("conectado!");
    mqttPublicar(topicoStatus, "online", true);
    mqttAssinar(topicoComando);
    publicarEstado("Fechado");
    if (ultimoNivel[0] != '\0') mqttPublicar(topicoNivel, ultimoNivel, true);
  } else {
    Serial.println("falhou. Nova tentativa em 5 s.");
  }
}

// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Iniciando alimentador...");

  // Serial2: conversa com o Uno a 9600 (mesma velocidade do Uno)
  Serial2.begin(9600, SERIAL_8N1, PINO_RX2, PINO_TX2);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  // Potência menor no Wi-Fi = pico de corrente menor (evita "Brownout").
  // O alcance diminui um pouco; fiquem perto do roteador.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.printf("Conectando ao Wi-Fi \"%s\"...\n", WIFI_SSID);
  ultimaTentativaWifi = millis();
}

void loop() {
  verificarWifi();
  verificarMqtt();

  if (mqttConectado) {
    mqttReceber();      // trata mensagens que chegaram
    mqttManterVivo();   // ping para o broker saber que estamos vivos
  }

  lerUno();  // mensagens do Uno: distância e estado da tampa
}
