#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ============================================================
// CONFIGURACOES
// ============================================================

#define DHTPIN 4
#define DHTTYPE DHT22
#define LDR_PIN 5

const char* AP_NOME = "ConservaAI-Config";
const char* AP_SENHA = "12345678";

const char* URL_CONSERVAAI_NUVEM =
  "https://script.google.com/macros/s/AKfycbylthUafCzcikk_-bvHItxu37hcYVWOSvoI3fzduZ2ZaoB6cQsnlKWFjI9y_AldmLLsHw/exec";

const char* ID_DISPOSITIVO = "CONSERVAAI-ESP32-01";

// ============================================================
// OBJETOS
// ============================================================

DHT dht(DHTPIN, DHTTYPE);

WebServer servidor(80);
DNSServer dnsServer;
Preferences preferencias;

// ============================================================
// WIFI
// ============================================================

const int MAX_REDES_WIFI = 5;

struct PerfilWiFi {
  String ssid;
  String senha;
};

PerfilWiFi redesWiFi[MAX_REDES_WIFI];

String redeSalva = "";
String senhaSalva = "";

bool wifiConfigurado = false;
bool portalAtivo = false;

// MODO DE CONEXAO:
// 0 = automatico (tenta as redes cadastradas na ordem)
// 1 = manual (usa somente a rede escolhida)
int modoConexaoWiFi = 0;

// Indice da rede escolhida manualmente: 0 a 4.
// -1 significa nenhuma rede escolhida.
int redeSelecionadaWiFi = -1;

// Solicita uma nova conexao fora do handler HTTP.
// Isso evita trocar de AP para STA no meio da resposta do navegador.
bool conexaoWiFiSolicitada = false;
int redeSolicitadaWiFi = -1;
int modoSolicitadoWiFi = 0;

// ============================================================
// NTP
// ============================================================

const char* SERVIDOR_NTP = "pool.ntp.org";

const long GMT_OFFSET = -3 * 3600;
const int DST_OFFSET = 0;

bool horarioSincronizado = false;

// ============================================================
// DADOS DOS SENSORES
// ============================================================

float temperaturaAtual = NAN;
float umidadeAtual = NAN;

bool poucaLuz = true;

unsigned long ultimaLeituraSensor = 0;
unsigned long ultimoRegistro = 0;

const unsigned long INTERVALO_SENSOR = 2000;
const unsigned long INTERVALO_REGISTRO = 10000;

// ============================================================
// CONTEXTO DO MONITORAMENTO
// ============================================================

String loteAtual = "LOTE-ESP32-01";
String localAtual = "LOCAL-001";
String usuarioAtual = "USUARIO-001";
String monitoramentoAtual = "MON-ESP32-01";

// ============================================================
// NUVEM
// ============================================================

bool nuvemAtiva = false;

String ultimoEnvioNuvem =
  "Aguardando primeiro envio.";

// ============================================================
// CULTURAS
// ============================================================

enum Produto {
  ALFACE,
  TOMATE,
  CEBOLA
};

struct PerfilCultura {

  String nome;
  String estagio;

  float temperaturaMin;
  float temperaturaMax;

  float umidadeMin;
  float umidadeMax;
};

// ============================================================
// PERFIS PROVISORIOS
// ============================================================

PerfilCultura perfilAlface = {

  "ALFACE",
  "",

  5.0,
  10.0,

  85.0,
  95.0
};

PerfilCultura perfilTomate = {

  "TOMATE",
  "",

  10.0,
  15.0,

  85.0,
  95.0
};

PerfilCultura perfilCebola = {

  "CEBOLA",
  "",

  0.0,
  5.0,

  65.0,
  70.0
};

// ============================================================
// HISTORICO LOCAL
// ============================================================

struct Registro {

  String dataHora;

  String dispositivo;
  String usuario;
  String local;
  String lote;

  String produto;
  String estagio;

  float temperatura;
  float umidade;

  String luz;

  String status;
  String motivo;
};

const int MAX_REGISTROS = 50;

Registro historico[MAX_REGISTROS];

int quantidadeRegistros = 0;

// ============================================================
// CONTROLE DE ID
// ============================================================

unsigned long contadorMedicao = 0;

// ============================================================
// CONTROLE DE LIMPEZA INICIAL
// ============================================================

// Esta chave identifica especificamente a limpeza desta
// validacao. Depois que for executada uma vez, nao sera
// executada novamente nos proximos reinicios.

const char* NAMESPACE_SISTEMA = "sistema";
const char* CHAVE_LIMPEZA = "limpeza_v1";

bool modoAposLimpeza = false;

// ============================================================
// UTILIDADES
// ============================================================

String obterDataHora() {

  struct tm tempo;

  if (!getLocalTime(&tempo)) {

    return "DATA-NAO-DISPONIVEL";
  }

  char buffer[25];

  strftime(
    buffer,
    sizeof(buffer),
    "%Y-%m-%d %H:%M:%S",
    &tempo
  );

  return String(buffer);
}

// ============================================================
// ID DA MEDICAO
// ============================================================

String gerarIdMedicao() {

  contadorMedicao++;

  struct tm tempo;

  if (getLocalTime(&tempo)) {

    char buffer[20];

    strftime(
      buffer,
      sizeof(buffer),
      "%Y%m%d%H%M%S",
      &tempo
    );

    return
      "MED-" +
      String(buffer) +
      "-" +
      String(contadorMedicao);
  }

  return
    "MED-" +
    String(millis()) +
    "-" +
    String(contadorMedicao);
}

// ============================================================
// ID DA AVALIACAO
// ============================================================

String gerarIdAvaliacao() {

  contadorMedicao++;

  return
    "AVL-" +
    String(millis()) +
    "-" +
    String(contadorMedicao);
}

// ============================================================
// URL ENCODE
// ============================================================

String urlEncode(String texto) {

  String resultado = "";

  const char* hex =
    "0123456789ABCDEF";

  for (
    unsigned int i = 0;
    i < texto.length();
    i++
  ) {

    char c = texto.charAt(i);

    if (
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' ||
      c == '_' ||
      c == '.' ||
      c == '~'
    ) {

      resultado += c;

    } else {

      resultado += '%';

      resultado +=
        hex[(c >> 4) & 0x0F];

      resultado +=
        hex[c & 0x0F];
    }
  }

  return resultado;
}

// ============================================================
// PERFIL
// ============================================================

PerfilCultura obterPerfil(
  Produto produto
) {

  if (produto == ALFACE) {

    return perfilAlface;
  }

  if (produto == TOMATE) {

    return perfilTomate;
  }

  return perfilCebola;
}

// ============================================================
// AVALIACAO
// ============================================================

String obterStatus(

  PerfilCultura perfil,

  float temperatura,
  float umidade,

  String &motivo
) {

  bool temperaturaBaixa =
    temperatura < perfil.temperaturaMin;

  bool temperaturaAlta =
    temperatura > perfil.temperaturaMax;

  bool umidadeBaixa =
    umidade < perfil.umidadeMin;

  bool umidadeAlta =
    umidade > perfil.umidadeMax;

  motivo = "";

  if (temperaturaBaixa) {

    motivo +=
      "Temperatura muito baixa";
  }

  if (temperaturaAlta) {

    if (motivo.length() > 0) {

      motivo += " | ";
    }

    motivo +=
      "Temperatura muito alta";
  }

  if (umidadeBaixa) {

    if (motivo.length() > 0) {

      motivo += " | ";
    }

    motivo +=
      "Umidade muito baixa";
  }

  if (umidadeAlta) {

    if (motivo.length() > 0) {

      motivo += " | ";
    }

    motivo +=
      "Umidade muito alta";
  }

  if (motivo.length() > 0) {

    return "CRITICO";
  }

  return "IDEAL";
}

// ============================================================
// LITTLEFS
// ============================================================

void iniciarLittleFS() {

  if (!LittleFS.begin(true)) {

    Serial.println(
      "[LITTLEFS] Erro ao iniciar memoria."
    );

    return;
  }

  Serial.println(
    "[LITTLEFS] Memoria permanente iniciada."
  );
}

// ============================================================
// LIMPEZA UNICA DO HISTORICO
// ============================================================

bool executarLimpezaInicialUmaVez() {

  preferencias.begin(
    NAMESPACE_SISTEMA,
    false
  );

  bool limpezaJaExecutada =
    preferencias.getBool(
      CHAVE_LIMPEZA,
      false
    );

  if (limpezaJaExecutada) {

    preferencias.end();

    return false;
  }

  Serial.println();
  Serial.println(
    "============================================"
  );

  Serial.println(
    "       LIMPEZA INICIAL DE VALIDACAO"
  );

  Serial.println(
    "============================================"
  );

  Serial.println(
    "[LIMPEZA] Apagando historico local..."
  );

  if (
    LittleFS.exists(
      "/historico.txt"
    )
  ) {

    if (
      LittleFS.remove(
        "/historico.txt"
      )
    ) {

      Serial.println(
        "[LIMPEZA] Arquivo historico.txt removido."
      );

    } else {

      Serial.println(
        "[LIMPEZA] Nao foi possivel remover o arquivo."
      );
    }

  } else {

    Serial.println(
      "[LIMPEZA] Nenhum historico encontrado."
    );
  }

  quantidadeRegistros = 0;
  contadorMedicao = 0;

  preferencias.putBool(
    CHAVE_LIMPEZA,
    true
  );

  preferencias.end();

  Serial.println(
    "[LIMPEZA] Registros locais: 0"
  );

  Serial.println(
    "[LIMPEZA] Wi-Fi preservado."
  );

  Serial.println(
    "[LIMPEZA] Configuracoes do sistema preservadas."
  );

  Serial.println();
  Serial.println(
    "============================================"
  );

  Serial.println(
    "       LIMPEZA CONCLUIDA COM SUCESSO"
  );

  Serial.println(
    "============================================"
  );

  Serial.println(
    "O ESP32 NAO iniciara o monitoramento agora."
  );

  Serial.println();
  Serial.println(
    "Reinicie o ESP32 para iniciar a nova"
  );

  Serial.println(
    "validacao com o historico zerado."
  );

  Serial.println(
    "============================================"
  );

  return true;
}

// ============================================================
// SALVAR REGISTRO LOCAL
// ============================================================

void salvarRegistroLocal(
  Registro registro
) {

  File arquivo =
    LittleFS.open(
      "/historico.txt",
      FILE_APPEND
    );

  if (!arquivo) {

    Serial.println(
      "[HISTORICO] Erro ao abrir arquivo."
    );

    return;
  }

  arquivo.println(

    registro.dataHora + ";" +

    registro.dispositivo + ";" +

    registro.usuario + ";" +

    registro.local + ";" +

    registro.lote + ";" +

    registro.produto + ";" +

    registro.estagio + ";" +

    String(
      registro.temperatura,
      1
    ) + ";" +

    String(
      registro.umidade,
      1
    ) + ";" +

    registro.luz + ";" +

    registro.status + ";" +

    registro.motivo
  );

  arquivo.close();
}

// ============================================================
// CARREGAR HISTORICO
// ============================================================

void carregarHistorico() {

  quantidadeRegistros = 0;

  if (
    !LittleFS.exists(
      "/historico.txt"
    )
  ) {

    Serial.println(
      "[LITTLEFS] Nenhum registro local encontrado."
    );

    return;
  }

  File arquivo =
    LittleFS.open(
      "/historico.txt",
      FILE_READ
    );

  if (!arquivo) {

    return;
  }

  while (

    arquivo.available() &&

    quantidadeRegistros < MAX_REGISTROS

  ) {

    String linha =
      arquivo.readStringUntil('\n');

    linha.trim();

    if (linha.length() == 0) {

      continue;
    }

    quantidadeRegistros++;
  }

  arquivo.close();

  Serial.print(
    "[LITTLEFS] Registros recuperados: "
  );

  Serial.println(
    quantidadeRegistros
  );
}

// ============================================================
// LIMPAR HISTORICO MANUALMENTE
// ============================================================

void limparHistorico() {

  if (
    LittleFS.exists(
      "/historico.txt"
    )
  ) {

    LittleFS.remove(
      "/historico.txt"
    );
  }

  quantidadeRegistros = 0;

  Serial.println(
    "[HISTORICO] Historico local apagado."
  );
}

// ============================================================
// WIFI
// ============================================================

void carregarWiFi() {

  preferencias.begin("wifi", true);

  wifiConfigurado = false;

  bool existeRedeNova = false;

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    String chaveSSID = "ssid" + String(i + 1);
    String chaveSenha = "senha" + String(i + 1);

    if (preferencias.isKey(chaveSSID.c_str())) {
      redesWiFi[i].ssid =
        preferencias.getString(chaveSSID.c_str(), "");
    } else {
      redesWiFi[i].ssid = "";
    }

    if (preferencias.isKey(chaveSenha.c_str())) {
      redesWiFi[i].senha =
        preferencias.getString(chaveSenha.c_str(), "");
    } else {
      redesWiFi[i].senha = "";
    }

    if (redesWiFi[i].ssid.length() > 0) {
      existeRedeNova = true;
      wifiConfigurado = true;
    }
  }

  // Compatibilidade com a versao anterior de uma unica rede.
  if (!existeRedeNova) {

    String ssidAntigo = "";
    String senhaAntiga = "";

    if (preferencias.isKey("ssid")) {
      ssidAntigo = preferencias.getString("ssid", "");
    }

    if (preferencias.isKey("senha")) {
      senhaAntiga = preferencias.getString("senha", "");
    }

    if (ssidAntigo.length() > 0) {

      redesWiFi[0].ssid = ssidAntigo;
      redesWiFi[0].senha = senhaAntiga;
      wifiConfigurado = true;

      Serial.println("[WIFI] Configuracao antiga encontrada.");
      Serial.println("[WIFI] Rede antiga sera usada como Rede 1.");
    }
  }

  // Carrega o modo de conexao salvo.
  modoConexaoWiFi =
    preferencias.getInt("modo", 0);

  if (
    modoConexaoWiFi != 0 &&
    modoConexaoWiFi != 1
  ) {
    modoConexaoWiFi = 0;
  }

  // Carrega a rede escolhida manualmente.
  redeSelecionadaWiFi =
    preferencias.getInt("rede_sel", -1);

  if (
    redeSelecionadaWiFi < 0 ||
    redeSelecionadaWiFi >= MAX_REDES_WIFI ||
    redesWiFi[redeSelecionadaWiFi].ssid.length() == 0
  ) {
    redeSelecionadaWiFi = -1;
  }

  preferencias.end();

  // Se estiver no modo manual, mas nao houver rede valida
  // selecionada, volta para o modo automatico.
  if (
    modoConexaoWiFi == 1 &&
    redeSelecionadaWiFi == -1
  ) {
    modoConexaoWiFi = 0;
  }

  redeSalva = "";
  senhaSalva = "";

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    if (redesWiFi[i].ssid.length() > 0) {
      redeSalva = redesWiFi[i].ssid;
      senhaSalva = redesWiFi[i].senha;
      break;
    }
  }

  Serial.println();
  Serial.println("[WIFI] Redes salvas:");

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    Serial.print("  ");
    Serial.print(i + 1);
    Serial.print(": ");

    if (redesWiFi[i].ssid.length() > 0) {
      Serial.println(redesWiFi[i].ssid);
    } else {
      Serial.println("(vazia)");
    }
  }

  Serial.print("[WIFI] Modo: ");

  if (modoConexaoWiFi == 1) {
    Serial.println("MANUAL");
  } else {
    Serial.println("AUTOMATICO");
  }

  Serial.print("[WIFI] Rede selecionada: ");

  if (redeSelecionadaWiFi >= 0) {
    Serial.print(redeSelecionadaWiFi + 1);
    Serial.print(" - ");
    Serial.println(
      redesWiFi[redeSelecionadaWiFi].ssid
    );
  } else {
    Serial.println("nenhuma");
  }
}

// ============================================================
// CONECTAR EM UMA REDE ESPECIFICA
// ============================================================

bool conectarWiFiRede(int indice) {

  if (
    indice < 0 ||
    indice >= MAX_REDES_WIFI
  ) {
    return false;
  }

  if (
    redesWiFi[indice].ssid.length() == 0
  ) {
    return false;
  }

  Serial.println();
  Serial.println("============================================");
  Serial.println("          CONEXAO WI-FI MANUAL");
  Serial.println("============================================");

  Serial.print("[WIFI] Rede escolhida: ");
  Serial.print(indice + 1);
  Serial.print(" - ");
  Serial.println(redesWiFi[indice].ssid);

  portalAtivo = false;

  WiFi.mode(WIFI_STA);
  delay(300);

  WiFi.disconnect(false, false);
  delay(300);

  WiFi.begin(
    redesWiFi[indice].ssid.c_str(),
    redesWiFi[indice].senha.c_str()
  );

  int tentativas = 0;

  while (
    WiFi.status() != WL_CONNECTED &&
    tentativas < 20
  ) {

    delay(500);
    Serial.print(".");
    tentativas++;
  }

  Serial.println();

  if (
    WiFi.status() == WL_CONNECTED
  ) {

    redeSalva =
      redesWiFi[indice].ssid;

    senhaSalva =
      redesWiFi[indice].senha;

    nuvemAtiva = true;

    Serial.println(
      "[WIFI] Conectado com sucesso."
    );

    Serial.print(
      "[WIFI] Rede utilizada: "
    );

    Serial.println(
      redeSalva
    );

    Serial.print(
      "[WIFI] Endereco IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.println(
      "============================================"
    );

    return true;
  }

  nuvemAtiva = false;

  Serial.println(
    "[WIFI] Falha ao conectar na rede escolhida."
  );

  Serial.println(
    "============================================"
  );

  return false;
}

// ============================================================
// CONEXAO AUTOMATICA
// ============================================================

bool conectarWiFiAutomatico() {

  if (!wifiConfigurado) {

    Serial.println(
      "[WIFI] Nenhuma rede cadastrada."
    );

    return false;
  }

  Serial.println();
  Serial.println("============================================");
  Serial.println("       TENTANDO REDES WI-FI SALVAS");
  Serial.println("       MODO AUTOMATICO");
  Serial.println("============================================");

  WiFi.mode(WIFI_STA);

  for (
    int i = 0;
    i < MAX_REDES_WIFI;
    i++
  ) {

    if (
      redesWiFi[i].ssid.length() == 0
    ) {
      continue;
    }

    Serial.print(
      "[WIFI] Tentando rede "
    );

    Serial.print(
      i + 1
    );

    Serial.print(
      ": "
    );

    Serial.println(
      redesWiFi[i].ssid
    );

    WiFi.disconnect(
      false,
      false
    );

    delay(300);

    WiFi.begin(
      redesWiFi[i].ssid.c_str(),
      redesWiFi[i].senha.c_str()
    );

    int tentativas = 0;

    while (
      WiFi.status() != WL_CONNECTED &&
      tentativas < 20
    ) {

      delay(500);
      Serial.print(".");
      tentativas++;
    }

    Serial.println();

    if (
      WiFi.status() == WL_CONNECTED
    ) {

      redeSalva =
        redesWiFi[i].ssid;

      senhaSalva =
        redesWiFi[i].senha;

      nuvemAtiva = true;

      Serial.println(
        "[WIFI] Conectado com sucesso."
      );

      Serial.print(
        "[WIFI] Rede utilizada: "
      );

      Serial.println(
        redeSalva
      );

      Serial.print(
        "[WIFI] Endereco IP: "
      );

      Serial.println(
        WiFi.localIP()
      );

      Serial.println(
        "============================================"
      );

      return true;
    }

    Serial.println(
      "[WIFI] Falha nesta rede."
    );
  }

  WiFi.disconnect(
    false,
    false
  );

  nuvemAtiva = false;

  Serial.println(
    "[WIFI] Nenhuma rede salva conseguiu conexao."
  );

  Serial.println(
    "[WIFI] Use o comando W para abrir o portal."
  );

  return false;
}

// ============================================================
// CONEXAO DE ACORDO COM O MODO SELECIONADO
// ============================================================

bool conectarWiFi() {

  if (!wifiConfigurado) {

    Serial.println(
      "[WIFI] Nenhuma rede cadastrada."
    );

    return false;
  }

  if (
    modoConexaoWiFi == 1
  ) {

    if (
      redeSelecionadaWiFi >= 0
    ) {

      return conectarWiFiRede(
        redeSelecionadaWiFi
      );
    }

    Serial.println(
      "[WIFI] Modo manual sem rede selecionada."
    );

    return false;
  }

  return conectarWiFiAutomatico();
}

// ============================================================
// DECLARACAO ANTECIPADA
// ============================================================

void sincronizarHorario();

// ============================================================
// PROCESSAR CONEXAO SOLICITADA PELO PORTAL
// ============================================================

void processarConexaoWiFiSolicitada() {

  if (!conexaoWiFiSolicitada) {
    return;
  }

  int indice = redeSolicitadaWiFi;
  int modo = modoSolicitadoWiFi;

  conexaoWiFiSolicitada = false;
  redeSolicitadaWiFi = -1;

  modoConexaoWiFi = modo;

  if (
    modo == 1 &&
    indice >= 0 &&
    indice < MAX_REDES_WIFI
  ) {

    redeSelecionadaWiFi = indice;

  } else if (
    modo == 0
  ) {

    redeSelecionadaWiFi = -1;
  }

  preferencias.begin(
    "wifi",
    false
  );

  preferencias.putInt(
    "modo",
    modoConexaoWiFi
  );

  preferencias.putInt(
    "rede_sel",
    redeSelecionadaWiFi
  );

  preferencias.end();

  bool sucesso = false;

  if (
    modoConexaoWiFi == 1 &&
    redeSelecionadaWiFi >= 0
  ) {

    sucesso =
      conectarWiFiRede(
        redeSelecionadaWiFi
      );

  } else {

    sucesso =
      conectarWiFiAutomatico();
  }

  if (sucesso) {

    Serial.println(
      "[WIFI] Nova configuracao aplicada."
    );

    if (
      WiFi.status() == WL_CONNECTED
    ) {

      sincronizarHorario();
    }

  } else {

    Serial.println(
      "[WIFI] Nova configuracao nao conseguiu conexao."
    );
  }
}

// ============================================================
// NTP
// ============================================================

void sincronizarHorario() {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return;
  }

  Serial.println(
    "[NTP] Sincronizando horario"
  );

  configTime(
    GMT_OFFSET,
    DST_OFFSET,
    SERVIDOR_NTP
  );

  struct tm tempo;

  int tentativas = 0;

  while (

    !getLocalTime(&tempo) &&

    tentativas < 20

  ) {

    delay(500);

    Serial.print(".");

    tentativas++;
  }

  Serial.println();

  if (
    getLocalTime(&tempo)
  ) {

    horarioSincronizado = true;

    Serial.println(
      "[NTP] Horario sincronizado."
    );

  } else {

    Serial.println(
      "[NTP] Nao foi possivel sincronizar."
    );
  }
}

// ============================================================
// REGISTRAR LOTE NA NUVEM
// ============================================================

bool registrarLoteNuvem() {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return false;
  }

  WiFiClientSecure cliente;

  cliente.setInsecure();

  HTTPClient http;

  String url =
    String(
      URL_CONSERVAAI_NUVEM
    ) +

    "?acao=registrar_lote" +

    "&lote_id=" +
    urlEncode(loteAtual) +

    "&produto=AMBIENTE" +

    "&responsavel_tipo=Produtor";

  Serial.println(
    "[NUVEM] Registrando lote..."
  );

  Serial.print(
    "[NUVEM] Lote: "
  );

  Serial.println(
    loteAtual
  );

  http.setFollowRedirects(
    HTTPC_STRICT_FOLLOW_REDIRECTS
  );

  if (
    !http.begin(
      cliente,
      url
    )
  ) {

    Serial.println(
      "[NUVEM] Falha ao iniciar conexao."
    );

    return false;
  }

  int codigo =
    http.GET();

  Serial.print(
    "[NUVEM] HTTP "
  );

  Serial.println(
    codigo
  );

  bool sucesso =
    codigo >= 200 &&
    codigo < 300;

  http.end();

  if (sucesso) {

    Serial.println(
      "[NUVEM] Lote registrado."
    );
  }

  return sucesso;
}

// ============================================================
// REGISTRAR MONITORAMENTO
// ============================================================

bool registrarMonitoramentoNuvem() {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return false;
  }

  WiFiClientSecure cliente;

  cliente.setInsecure();

  HTTPClient http;

  String url =
    String(
      URL_CONSERVAAI_NUVEM
    ) +

    "?acao=registrar_monitoramento" +

    "&monitoramento_id=" +
    urlEncode(monitoramentoAtual) +

    "&lote_id=" +
    urlEncode(loteAtual) +

    "&dispositivo_id=" +
    urlEncode(ID_DISPOSITIVO) +

    "&local_id=" +
    urlEncode(localAtual);

  Serial.println(
    "[NUVEM] Registrando monitoramento..."
  );

  Serial.print(
    "[NUVEM] Monitoramento: "
  );

  Serial.println(
    monitoramentoAtual
  );

  http.setFollowRedirects(
    HTTPC_STRICT_FOLLOW_REDIRECTS
  );

  if (
    !http.begin(
      cliente,
      url
    )
  ) {

    Serial.println(
      "[NUVEM] Falha ao iniciar conexao."
    );

    return false;
  }

  int codigo =
    http.GET();

  Serial.print(
    "[NUVEM] HTTP "
  );

  Serial.println(
    codigo
  );

  bool sucesso =
    codigo >= 200 &&
    codigo < 300;

  http.end();

  if (sucesso) {

    Serial.println(
      "[NUVEM] Monitoramento registrado."
    );
  }

  return sucesso;
}

// ============================================================
// ENVIO DE MEDICAO
// ============================================================

bool enviarMedicaoNuvem(

  String idMedicao,

  String dataHora,

  float temperatura,

  float umidade,

  String luz
) {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return false;
  }

  WiFiClientSecure cliente;

  cliente.setInsecure();

  HTTPClient http;

  String url =
    String(
      URL_CONSERVAAI_NUVEM
    ) +

    "?acao=registrar_medicao" +

    "&id_medicao=" +
    urlEncode(idMedicao) +

    "&monitoramento_id=" +
    urlEncode(monitoramentoAtual) +

    "&data_hora_medicao=" +
    urlEncode(dataHora) +

    "&dispositivo_id=" +
    urlEncode(ID_DISPOSITIVO) +

    "&usuario_id=" +
    urlEncode(usuarioAtual) +

    "&local_id=" +
    urlEncode(localAtual) +

    "&lote_id=" +
    urlEncode(loteAtual) +

    "&temperatura=" +
    urlEncode(
      String(
        temperatura,
        1
      )
    ) +

    "&umidade=" +
    urlEncode(
      String(
        umidade,
        1
      )
    ) +

    "&luz=" +
    urlEncode(luz);

  Serial.println(
    "[NUVEM] Enviando MEDICAO..."
  );

  http.setFollowRedirects(
    HTTPC_STRICT_FOLLOW_REDIRECTS
  );

  if (
    !http.begin(
      cliente,
      url
    )
  ) {

    Serial.println(
      "[NUVEM] Falha ao iniciar conexao."
    );

    return false;
  }

  int codigo =
    http.GET();

  Serial.print(
    "[NUVEM] HTTP "
  );

  Serial.println(
    codigo
  );

  bool sucesso =
    codigo >= 200 &&
    codigo < 300;

  http.end();

  if (sucesso) {

    Serial.println(
      "[NUVEM] Medicao enviada com sucesso."
    );

  } else {

    Serial.println(
      "[NUVEM] Falha ao enviar medicao."
    );
  }

  return sucesso;
}

// ============================================================
// ENVIO DE AVALIACAO
// ============================================================

bool enviarAvaliacaoNuvem(

  String idMedicao,

  PerfilCultura perfil,

  String status,

  String motivo
) {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    return false;
  }

  WiFiClientSecure cliente;

  cliente.setInsecure();

  HTTPClient http;

  String url =
    String(
      URL_CONSERVAAI_NUVEM
    ) +

    "?acao=registrar_avaliacao" +

    "&id_avaliacao=" +
    urlEncode(
      gerarIdAvaliacao()
    ) +

    "&id_medicao=" +
    urlEncode(idMedicao) +

    "&monitoramento_id=" +
    urlEncode(monitoramentoAtual) +

    "&produto=" +
    urlEncode(perfil.nome) +

    "&estagio=" +
    urlEncode("") +

    "&status=" +
    urlEncode(status) +

    "&motivo=" +
    urlEncode(motivo) +

    "&perfil_versao=" +
    urlEncode(
      perfil.nome + "-V1"
    );

  Serial.print(
    "[NUVEM] Enviando avaliacao: "
  );

  Serial.println(
    perfil.nome
  );

  http.setFollowRedirects(
    HTTPC_STRICT_FOLLOW_REDIRECTS
  );

  if (
    !http.begin(
      cliente,
      url
    )
  ) {

    Serial.println(
      "[NUVEM] Falha ao iniciar conexao."
    );

    return false;
  }

  int codigo =
    http.GET();

  Serial.print(
    "[NUVEM] HTTP "
  );

  Serial.println(
    codigo
  );

  bool sucesso =
    codigo >= 200 &&
    codigo < 300;

  http.end();

  if (sucesso) {

    Serial.print(
      "[NUVEM] Avaliacao "
    );

    Serial.print(
      perfil.nome
    );

    Serial.println(
      " enviada com sucesso."
    );

  } else {

    Serial.print(
      "[NUVEM] Falha na avaliacao "
    );

    Serial.println(
      perfil.nome
    );
  }

  return sucesso;
}

// ============================================================
// REGISTRO LOCAL + NUVEM
// ============================================================

void registrarMonitoramento() {

  if (

    isnan(temperaturaAtual) ||

    isnan(umidadeAtual)

  ) {

    return;
  }

  String dataHora =
    obterDataHora();

  String luz =
    poucaLuz
      ? "POUCA LUZ"
      : "LUZ";

  String idMedicao =
    gerarIdMedicao();

  // ----------------------------------------------------------
  // 1. MEDICAO LOCAL
  // ----------------------------------------------------------

  Registro base;

  base.dataHora =
    dataHora;

  base.dispositivo =
    ID_DISPOSITIVO;

  base.usuario =
    usuarioAtual;

  base.local =
    localAtual;

  base.lote =
    loteAtual;

  base.produto =
    "AMBIENTE";

  base.estagio =
    "";

  base.temperatura =
    temperaturaAtual;

  base.umidade =
    umidadeAtual;

  base.luz =
    luz;

  base.status =
    "MEDICAO";

  base.motivo =
    "Leitura dos sensores";

  salvarRegistroLocal(
    base
  );

  quantidadeRegistros++;

  if (
    quantidadeRegistros > MAX_REGISTROS
  ) {

    quantidadeRegistros =
      MAX_REGISTROS;
  }

  Serial.println();

  Serial.println(
    "[HISTORICO] Nova medicao salva."
  );

  // ----------------------------------------------------------
  // 2. MEDICAO NA NUVEM
  // ----------------------------------------------------------

  if (nuvemAtiva) {

    bool enviada =
      enviarMedicaoNuvem(

        idMedicao,

        dataHora,

        temperaturaAtual,

        umidadeAtual,

        luz
      );

    if (!enviada) {

      Serial.println(
        "[NUVEM] Medicao nao enviada."
      );
    }
  }

  // ----------------------------------------------------------
  // 3. AVALIAR AS TRES CULTURAS
  // ----------------------------------------------------------

  PerfilCultura perfis[3] = {

    perfilAlface,
    perfilTomate,
    perfilCebola
  };

  for (
    int i = 0;
    i < 3;
    i++
  ) {

    String motivo;

    String status =
      obterStatus(

        perfis[i],

        temperaturaAtual,

        umidadeAtual,

        motivo
      );

    // --------------------------------------------------------
    // SERIAL
    // --------------------------------------------------------

    Serial.println();

    Serial.println(
      "------------- AVALIACAO ----------------"
    );

    Serial.print(
      "Produto: "
    );

    Serial.println(
      perfis[i].nome
    );

    Serial.print(
      "Lote: "
    );

    Serial.println(
      loteAtual
    );

    Serial.print(
      "Monitoramento: "
    );

    Serial.println(
      monitoramentoAtual
    );

    Serial.print(
      "ID Medicao: "
    );

    Serial.println(
      idMedicao
    );

    Serial.print(
      "Temperatura: "
    );

    Serial.print(
      temperaturaAtual,
      1
    );

    Serial.println(
      " C"
    );

    Serial.print(
      "Umidade: "
    );

    Serial.print(
      umidadeAtual,
      1
    );

    Serial.println(
      " %"
    );

    Serial.print(
      "Luz: "
    );

    Serial.println(
      luz
    );

    Serial.print(
      "Status: "
    );

    Serial.println(
      status
    );

    Serial.print(
      "Motivo: "
    );

    if (
      motivo.length() > 0
    ) {

      Serial.println(
        motivo
      );

    } else {

      Serial.println(
        "Parametros dentro do perfil."
      );
    }

    Serial.println(
      "----------------------------------------"
    );

    // --------------------------------------------------------
    // NUVEM
    // --------------------------------------------------------

    if (nuvemAtiva) {

      enviarAvaliacaoNuvem(

        idMedicao,

        perfis[i],

        status,

        motivo
      );
    }
  }

  ultimoEnvioNuvem =
    dataHora;
}

// ============================================================
// LEITURA DOS SENSORES
// ============================================================

void lerSensores() {

  float novaTemperatura =
    dht.readTemperature();

  float novaUmidade =
    dht.readHumidity();

  if (

    !isnan(novaTemperatura) &&

    !isnan(novaUmidade)

  ) {

    temperaturaAtual =
      novaTemperatura;

    umidadeAtual =
      novaUmidade;
  }

  int leituraLDR =
    digitalRead(
      LDR_PIN
    );

  poucaLuz =
    leituraLDR == HIGH;
}

// ============================================================
// MONITORAMENTO SERIAL
// ============================================================

void mostrarMonitoramento() {

  if (

    isnan(temperaturaAtual) ||

    isnan(umidadeAtual)

  ) {

    return;
  }

  String luz =
    poucaLuz
      ? "POUCA LUZ"
      : "LUZ";

  Serial.println();

  Serial.println(
    "============================================"
  );

  Serial.println(
    "          MONITORAMENTO DO AMBIENTE"
  );

  Serial.println(
    "============================================"
  );

  Serial.print(
    "Data/Hora: "
  );

  Serial.println(
    obterDataHora()
  );

  Serial.print(
    "Dispositivo: "
  );

  Serial.println(
    ID_DISPOSITIVO
  );

  Serial.print(
    "Monitoramento: "
  );

  Serial.println(
    monitoramentoAtual
  );

  Serial.print(
    "Lote: "
  );

  Serial.println(
    loteAtual
  );

  Serial.print(
    "Temperatura: "
  );

  Serial.print(
    temperaturaAtual,
    1
  );

  Serial.println(
    " C"
  );

  Serial.print(
    "Umidade: "
  );

  Serial.print(
    umidadeAtual,
    1
  );

  Serial.println(
    " %"
  );

  Serial.print(
    "Luz: "
  );

  Serial.println(
    luz
  );

  PerfilCultura perfis[3] = {

    perfilAlface,
    perfilTomate,
    perfilCebola
  };

  for (
    int i = 0;
    i < 3;
    i++
  ) {

    String motivo;

    String status =
      obterStatus(

        perfis[i],

        temperaturaAtual,

        umidadeAtual,

        motivo
      );

    Serial.println();

    Serial.print(
      "------------- "
    );

    Serial.print(
      perfis[i].nome
    );

    Serial.println(
      " ----------------"
    );

    Serial.print(
      "Status: "
    );

    Serial.println(
      status
    );

    Serial.print(
      "Motivo: "
    );

    if (
      motivo.length() > 0
    ) {

      Serial.println(
        motivo
      );

    } else {

      Serial.println(
        "Parametros dentro do perfil."
      );
    }
  }

  Serial.println();

  Serial.print(
    "Nuvem: "
  );

  Serial.println(

    nuvemAtiva
      ? "ATIVA"
      : "INATIVA"
  );

  Serial.print(
    "Ultimo envio: "
  );

  Serial.println(
    ultimoEnvioNuvem
  );

  Serial.println(
    "============================================"
  );
}

// ============================================================
// INFORMACOES DOS PERFIS
// ============================================================

void mostrarPerfis() {

  Serial.println();

  Serial.println(
    "============================================"
  );

  Serial.println(
    "          PERFIS DAS CULTURAS"
  );

  Serial.println(
    "============================================"
  );

  PerfilCultura perfis[3] = {

    perfilAlface,
    perfilTomate,
    perfilCebola
  };

  for (
    int i = 0;
    i < 3;
    i++
  ) {

    Serial.println();

    Serial.print(
      perfis[i].nome
    );

    Serial.println(
      ":"
    );

    Serial.print(
      "Temperatura: "
    );

    Serial.print(
      perfis[i].temperaturaMin
    );

    Serial.print(
      " a "
    );

    Serial.print(
      perfis[i].temperaturaMax
    );

    Serial.println(
      " C"
    );

    Serial.print(
      "Umidade: "
    );

    Serial.print(
      perfis[i].umidadeMin
    );

    Serial.print(
      " a "
    );

    Serial.print(
      perfis[i].umidadeMax
    );

    Serial.println(
      " %"
    );
  }

  Serial.println(
    "============================================"
  );
}

// ============================================================
// INFORMACOES
// ============================================================

void mostrarInformacoes() {

  Serial.println();

  Serial.println(
    "============================================"
  );

  Serial.println(
    "       MONITORAMENTO AUTOMATICO"
  );

  Serial.println(
    "============================================"
  );

  Serial.println(
    "O sistema realiza uma leitura ambiental"
  );

  Serial.println(
    "usando DHT22 e LDR."
  );

  Serial.println();

  Serial.println(
    "A mesma medicao fisica e interpretada"
  );

  Serial.println(
    "para ALFACE, TOMATE e CEBOLA."
  );

  Serial.println();

  Serial.println(
    "Intervalo entre registros:"
  );

  Serial.println(
    "10 segundos."
  );

  Serial.println(
    "============================================"
  );
}

// ============================================================
// PORTAL WIFI
// ============================================================

void paginaWiFi() {

  String html =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ConservaAI - Wi-Fi</title>"
    "<style>"
    "body{font-family:Arial;padding:20px;max-width:760px;margin:auto;background:#f4f6f8;color:#222}"
    ".card{background:white;padding:22px;border-radius:15px;box-shadow:0 2px 10px rgba(0,0,0,.08)}"
    ".rede{border:1px solid #ddd;padding:15px;margin:12px 0;border-radius:10px;background:#fafafa}"
    ".status{padding:12px;border-radius:8px;background:#eef5ff;margin-bottom:15px}"
    ".btn{padding:11px 16px;border:0;border-radius:8px;cursor:pointer;margin:5px 5px 5px 0}"
    ".principal{background:#e8f5e9}"
    ".manual{background:#fff3cd}"
    "input{padding:10px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>ConservaAI</h1>"
    "<h2>Gerenciador de Wi-Fi</h2>";

  html +=
    "<div class='status'><b>Modo atual:</b> " +
    String(
      modoConexaoWiFi == 1
        ? "MANUAL"
        : "AUTOMATICO"
    ) +
    "<br><b>Rede atual:</b> " +
    (
      WiFi.status() == WL_CONNECTED
        ? htmlEscapar(WiFi.SSID())
        : String("Nao conectado")
    ) +
    "</div>";

  html +=
    "<form action='/salvarwifi' method='GET'>"
    "<h3>Modo de conexao</h3>";

  html +=
    "<label>"
    "<input type='radio' name='modo' value='0' " +
    String(
      modoConexaoWiFi == 0 ? "checked" : ""
    ) +
    "> "
    "<b>Automatico</b> — tenta as redes cadastradas na ordem."
    "</label><br><br>";

  html +=
    "<label>"
    "<input type='radio' name='modo' value='1' " +
    String(
      modoConexaoWiFi == 1 ? "checked" : ""
    ) +
    "> "
    "<b>Manual</b> — conecta somente na rede escolhida."
    "</label>";

  html +=
    "<h3>Redes cadastradas</h3>";

  for (
    int i = 0;
    i < MAX_REDES_WIFI;
    i++
  ) {

    html +=
      "<div class='rede'>"
      "<h3>Rede " +
      String(i + 1) +
      "</h3>"
      "<label>Nome da rede (SSID):</label><br>"
      "<input name='ssid" +
      String(i + 1) +
      "' value='" +
      htmlEscapar(redesWiFi[i].ssid) +
      "' style='width:100%'><br><br>"
      "<label>Senha:</label><br>"
      "<input name='senha" +
      String(i + 1) +
      "' type='password' value='" +
      htmlEscapar(redesWiFi[i].senha) +
      "' style='width:100%'><br><br>";

    if (
      redesWiFi[i].ssid.length() > 0
    ) {

      html +=
        "<label>"
        "<input type='radio' name='rede_sel' value='" +
        String(i) +
        "' " +
        String(
          redeSelecionadaWiFi == i
            ? "checked"
            : ""
        ) +
        "> "
        "Usar esta rede no modo manual"
        "</label>";
    } else {

      html +=
        "<span style='color:#777'>Posicao livre.</span>";
    }

    html +=
      "</div>";
  }

  html +=
    "<button class='btn principal' type='submit'>Salvar configuracao</button>"
    "</form>"
    "<hr>"
    "<h3>Conexao rapida</h3>"
    "<p>Escolha uma rede abaixo para testar a conexao imediatamente. "
    "A escolha tambem fica salva como rede manual.</p>";

  for (
    int i = 0;
    i < MAX_REDES_WIFI;
    i++
  ) {

    if (
      redesWiFi[i].ssid.length() == 0
    ) {
      continue;
    }

    html +=
      "<a href='/conectarwifi?rede=" +
      String(i) +
      "'>"
      "<button class='btn manual' type='button'>"
      "Conectar na Rede " +
      String(i + 1) +
      " — " +
      htmlEscapar(redesWiFi[i].ssid) +
      "</button>"
      "</a>";
  }

  html +=
    "<hr>"
    "<p><b>Importante:</b> deixe SSID e senha vazios "
    "para manter uma posicao sem rede.</p>"
    "</div>"
    "</body></html>";

  servidor.send(
    200,
    "text/html",
    html
  );
}

// ============================================================

void salvarWiFiWeb() {

  int redesSalvasAgora = 0;

  preferencias.begin(
    "wifi",
    false
  );

  for (
    int i = 0;
    i < MAX_REDES_WIFI;
    i++
  ) {

    String chaveSSID =
      "ssid" + String(i + 1);

    String chaveSenha =
      "senha" + String(i + 1);

    String ssid =
      servidor.arg(
        "ssid" + String(i + 1)
      );

    String senha =
      servidor.arg(
        "senha" + String(i + 1)
      );

    ssid.trim();

    redesWiFi[i].ssid =
      ssid;

    redesWiFi[i].senha =
      senha;

    preferencias.putString(
      chaveSSID.c_str(),
      ssid
    );

    preferencias.putString(
      chaveSenha.c_str(),
      senha
    );

    if (
      ssid.length() > 0
    ) {
      redesSalvasAgora++;
    }
  }

  String modoRecebido =
    servidor.arg("modo");

  int novoModo =
    modoRecebido == "1"
      ? 1
      : 0;

  int novaRedeSelecionada = -1;

  if (
    servidor.hasArg("rede_sel")
  ) {

    novaRedeSelecionada =
      servidor.arg("rede_sel").toInt();

    if (
      novaRedeSelecionada < 0 ||
      novaRedeSelecionada >= MAX_REDES_WIFI ||
      redesWiFi[novaRedeSelecionada].ssid.length() == 0
    ) {

      novaRedeSelecionada = -1;
    }
  }

  // No modo automatico, nao precisamos de uma rede
  // manual selecionada.
  if (
    novoModo == 0
  ) {

    novaRedeSelecionada = -1;
  }

  // Se o modo manual foi escolhido sem uma rede valida,
  // mantemos o modo automatico para evitar uma configuracao
  // sem destino.
  if (
    novoModo == 1 &&
    novaRedeSelecionada == -1
  ) {

    novoModo = 0;
  }

  preferencias.putInt(
    "modo",
    novoModo
  );

  preferencias.putInt(
    "rede_sel",
    novaRedeSelecionada
  );

  preferencias.end();

  wifiConfigurado =
    redesSalvasAgora > 0;

  modoConexaoWiFi =
    novoModo;

  redeSelecionadaWiFi =
    novaRedeSelecionada;

  redeSalva = "";
  senhaSalva = "";

  for (
    int i = 0;
    i < MAX_REDES_WIFI;
    i++
  ) {

    if (
      redesWiFi[i].ssid.length() > 0
    ) {

      redeSalva =
        redesWiFi[i].ssid;

      senhaSalva =
        redesWiFi[i].senha;

      break;
    }
  }

  // Apos salvar, prepara a conexao para ser feita
  // no loop principal, depois que a resposta HTTP
  // tiver sido enviada.
  conexaoWiFiSolicitada = true;

  if (
    novoModo == 1
  ) {

    redeSolicitadaWiFi =
      novaRedeSelecionada;

  } else {

    redeSolicitadaWiFi =
      -1;
  }

  modoSolicitadoWiFi =
    novoModo;

  String descricaoModo =
    novoModo == 1
      ? "MANUAL"
      : "AUTOMATICO";

  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ConservaAI - Wi-Fi</title>"
    "</head><body style='font-family:Arial;padding:20px;max-width:700px;margin:auto'>"
    "<h1>Configuracao salva</h1>"
    "<p>" +
    String(redesSalvasAgora) +
    " rede(s) cadastrada(s).</p>"
    "<p><b>Modo:</b> " +
    descricaoModo +
    "</p>";

  if (
    novoModo == 1 &&
    novaRedeSelecionada >= 0
  ) {

    html +=
      "<p><b>Rede escolhida:</b> Rede " +
      String(novaRedeSelecionada + 1) +
      " — " +
      htmlEscapar(
        redesWiFi[novaRedeSelecionada].ssid
      ) +
      "</p>";
  }

  html +=
    "<p>O ESP32 aplicara a nova configuracao "
    "assim que esta resposta terminar.</p>"
    "<p>Se a conexao for bem-sucedida, o endereco IP "
    "sera mostrado no monitor serial.</p>"
    "<a href='/'>Voltar para configuracao</a>"
    "</body></html>";

  servidor.send(
    200,
    "text/html",
    html
  );

  Serial.println();
  Serial.println(
    "[WIFI] Configuracao atualizada pelo portal."
  );

  Serial.print(
    "[WIFI] Redes cadastradas: "
  );

  Serial.println(
    redesSalvasAgora
  );

  Serial.print(
    "[WIFI] Modo selecionado: "
  );

  Serial.println(
    descricaoModo
  );

  if (
    novoModo == 1 &&
    novaRedeSelecionada >= 0
  ) {

    Serial.print(
      "[WIFI] Rede manual selecionada: "
    );

    Serial.print(
      novaRedeSelecionada + 1
    );

    Serial.print(
      " - "
    );

    Serial.println(
      redesWiFi[novaRedeSelecionada].ssid
    );
  }

  for (
    int i = 0;
    i < MAX_REDES_WIFI;
    i++
  ) {

    Serial.print(
      "  Rede "
    );

    Serial.print(
      i + 1
    );

    Serial.print(
      ": "
    );

    if (
      redesWiFi[i].ssid.length() > 0
    ) {

      Serial.println(
        redesWiFi[i].ssid
      );

    } else {

      Serial.println(
        "(vazia)"
      );
    }
  }
}

// ============================================================
// CONECTAR DIRETAMENTE EM UMA REDE PELO PORTAL
// ============================================================

void solicitarConexaoWiFiWeb() {

  if (
    !servidor.hasArg("rede")
  ) {

    servidor.send(
      400,
      "text/plain",
      "Rede nao informada."
    );

    return;
  }

  int indice =
    servidor.arg("rede").toInt();

  if (
    indice < 0 ||
    indice >= MAX_REDES_WIFI ||
    redesWiFi[indice].ssid.length() == 0
  ) {

    servidor.send(
      400,
      "text/plain",
      "Rede invalida."
    );

    return;
  }

  // Salva imediatamente a escolha manual.
  preferencias.begin(
    "wifi",
    false
  );

  preferencias.putInt(
    "modo",
    1
  );

  preferencias.putInt(
    "rede_sel",
    indice
  );

  preferencias.end();

  modoConexaoWiFi = 1;
  redeSelecionadaWiFi = indice;

  conexaoWiFiSolicitada = true;
  redeSolicitadaWiFi = indice;
  modoSolicitadoWiFi = 1;

  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='4;url=/'>"
    "<title>ConservaAI - Conectando</title>"
    "</head><body style='font-family:Arial;padding:20px;max-width:700px;margin:auto'>"
    "<h1>Conexao solicitada</h1>"
    "<p>O ESP32 tentara conectar na <b>Rede " +
    String(indice + 1) +
    "</b>: <b>" +
    htmlEscapar(redesWiFi[indice].ssid) +
    "</b>.</p>"
    "<p>Aguarde alguns segundos.</p>"
    "<p>Se a conexao for bem-sucedida, o ESP32 mudara "
    "para o modo normal e o ponto de acesso de configuracao sera encerrado.</p>"
    "</body></html>";

  servidor.send(
    200,
    "text/html",
    html
  );

  Serial.println();
  Serial.print(
    "[WIFI] Conexao manual solicitada para Rede "
  );
  Serial.println(
    indice + 1
  );
}

// ============================================================

void iniciarPortalWiFi() {

  portalAtivo = true;
  nuvemAtiva = false;

  WiFi.disconnect(false, false);
  delay(300);

  WiFi.mode(WIFI_AP);

  WiFi.softAP(AP_NOME, AP_SENHA);

  IPAddress ip = WiFi.softAPIP();

  Serial.println();
  Serial.println("============================================");
  Serial.println("          CONFIGURACAO WI-FI");
  Serial.println("============================================");
  Serial.print("Rede: ");
  Serial.println(AP_NOME);
  Serial.print("Senha: ");
  Serial.println(AP_SENHA);
  Serial.print("Endereco: ");
  Serial.println(ip);
  Serial.println();
  Serial.println("O portal permite cadastrar ate 5 redes.");
  Serial.println("============================================");
}

// ============================================================
// PAINEL WEB LOCAL
// ============================================================

void paginaPrincipal() {

  if (portalAtivo) {
    paginaWiFi();
    return;
  }

  String html =

    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ConservaAI</title>"

    "<style>"
    "body{font-family:Arial;margin:20px;background:#f4f6f8}"
    ".card{max-width:700px;margin:auto;background:white;padding:20px;border-radius:15px}"
    ".item{padding:12px;margin:8px 0;background:#f1f1f1;border-radius:8px}"
    "</style>"

    "</head>"

    "<body>"

    "<div class='card'>"

    "<h1>ConservaAI</h1>"
    "<h2>Monitoramento ambiental</h2>";

  if (
    !isnan(temperaturaAtual)
  ) {

    html +=

      "<div class='item'><b>Temperatura:</b> " +

      String(
        temperaturaAtual,
        1
      ) +

      " °C</div>";
  }

  if (
    !isnan(umidadeAtual)
  ) {

    html +=

      "<div class='item'><b>Umidade:</b> " +

      String(
        umidadeAtual,
        1
      ) +

      " %</div>";
  }

  html +=

    "<div class='item'><b>Luz:</b> " +

    String(
      poucaLuz
        ? "POUCA LUZ"
        : "LUZ"
    ) +

    "</div>";

  html +=

    "<div class='item'><b>Dispositivo:</b> " +

    String(
      ID_DISPOSITIVO
    ) +

    "</div>";

  html +=

    "<div class='item'><b>Monitoramento:</b> " +

    monitoramentoAtual +

    "</div>";

  html +=

    "<div class='item'><b>Lote:</b> " +

    loteAtual +

    "</div>";

  html +=

    "<div class='item'><b>Local:</b> " +

    localAtual +

    "</div>";

  html +=

    "</div>"
    "</body>"
    "</html>";

  servidor.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// ROTAS
// ============================================================

void configurarRotas() {

  servidor.on(
    "/",
    HTTP_GET,
    paginaPrincipal
  );

  servidor.on(
    "/salvarwifi",
    HTTP_GET,
    salvarWiFiWeb
  );

  servidor.on(
    "/conectarwifi",
    HTTP_GET,
    solicitarConexaoWiFiWeb
  );

  servidor.on(
    "/dados",
    HTTP_GET,
    []() {

      String resposta = "{";

      resposta +=
        "\"temperatura\":" +
        String(
          temperaturaAtual,
          1
        ) +
        ",";

      resposta +=
        "\"umidade\":" +
        String(
          umidadeAtual,
          1
        ) +
        ",";

      resposta +=
        "\"luz\":\"" +

        String(
          poucaLuz
            ? "POUCA LUZ"
            : "LUZ"
        ) +

        "\",";

      resposta +=
        "\"dispositivo\":\"" +

        String(
          ID_DISPOSITIVO
        ) +

        "\",";

      resposta +=
        "\"monitoramento\":\"" +

        monitoramentoAtual +

        "\",";

      resposta +=
        "\"lote\":\"" +

        loteAtual +

        "\"";

      resposta +=
        "}";

      servidor.send(
        200,
        "application/json",
        resposta
      );
    }
  );

  servidor.onNotFound(
    []() {

      servidor.send(
        404,
        "text/plain",
        "Pagina nao encontrada."
      );
    }
  );
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(
    115200
  );

  delay(1000);

  Serial.println();

  Serial.println(
    "============================================"
  );

  Serial.println(
    "              CONSERVAAI"
  );

  Serial.println(
    "============================================"
  );

  // ----------------------------------------------------------
  // LITTLEFS
  // ----------------------------------------------------------

  iniciarLittleFS();

  // ----------------------------------------------------------
  // LIMPEZA AUTOMATICA UMA UNICA VEZ
  // ----------------------------------------------------------

  if (
    executarLimpezaInicialUmaVez()
  ) {

    modoAposLimpeza = true;

    return;
  }

  // ----------------------------------------------------------
  // CARREGAR HISTORICO
  // ----------------------------------------------------------

  carregarHistorico();

  // ----------------------------------------------------------
  // SENSORES
  // ----------------------------------------------------------

  dht.begin();

  Serial.println(
    "[SENSORES] DHT22 iniciado."
  );

  pinMode(
    LDR_PIN,
    INPUT
  );

  Serial.println(
    "[SENSORES] LDR iniciado."
  );

  // ----------------------------------------------------------
  // WIFI
  // ----------------------------------------------------------

  carregarWiFi();

  bool conectadoWiFi = conectarWiFi();

  // ----------------------------------------------------------
  // SERVIDOR E PORTAL WI-FI
  // ----------------------------------------------------------

  configurarRotas();

  if (!conectadoWiFi && !wifiConfigurado) {

    Serial.println();
    Serial.println("[WIFI] Nenhuma rede cadastrada.");
    Serial.println("[WIFI] Abrindo automaticamente o portal ConservaAI-Config.");

    iniciarPortalWiFi();
  }

  servidor.begin();

  // ----------------------------------------------------------
  // NTP
  // ----------------------------------------------------------

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    sincronizarHorario();
  }

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println();

    Serial.println(
      "============================================"
    );

    Serial.println(
      "          PAINEL WEB ATIVO"
    );

    Serial.println(
      "============================================"
    );

    Serial.print(
      "Acesse pelo navegador: http://"
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.println(
      "============================================"
    );
  }

  // ----------------------------------------------------------
  // CONTEXTO
  // ----------------------------------------------------------

  Serial.println();

  Serial.println(
    "============================================"
  );

  Serial.println(
    "       INICIANDO CONTEXTO DO MONITORAMENTO"
  );

  Serial.println(
    "============================================"
  );

  Serial.print(
    "Dispositivo: "
  );

  Serial.println(
    ID_DISPOSITIVO
  );

  Serial.print(
    "Lote: "
  );

  Serial.println(
    loteAtual
  );

  Serial.print(
    "Monitoramento: "
  );

  Serial.println(
    monitoramentoAtual
  );

  Serial.print(
    "Local: "
  );

  Serial.println(
    localAtual
  );

  Serial.print(
    "Usuario: "
  );

  Serial.println(
    usuarioAtual
  );

  Serial.println(
    "Produtos avaliados:"
  );

  Serial.println(
    "- ALFACE"
  );

  Serial.println(
    "- TOMATE"
  );

  Serial.println(
    "- CEBOLA"
  );

  Serial.println(
    "============================================"
  );

  // ----------------------------------------------------------
  // REGISTRAR CONTEXTO NA NUVEM
  // ----------------------------------------------------------

  if (nuvemAtiva) {

    registrarLoteNuvem();

    registrarMonitoramentoNuvem();
  }

  // ----------------------------------------------------------
  // SISTEMA PRONTO
  // ----------------------------------------------------------

  Serial.println();

  Serial.println(
    "============================================"
  );

  Serial.println(
    "           SISTEMA PRONTO"
  );

  Serial.println(
    "============================================"
  );

  Serial.println(
    "Monitoramento automatico ativado."
  );

  Serial.println();

  Serial.println(
    "Produtos avaliados no mesmo ambiente:"
  );

  Serial.println(
    "- ALFACE"
  );

  Serial.println(
    "- TOMATE"
  );

  Serial.println(
    "- CEBOLA"
  );

  Serial.println();

  Serial.println(
    "Comandos:"
  );

  Serial.println(
    "H = Historico"
  );

  Serial.println(
    "P = Perfis das culturas"
  );

  Serial.println(
    "C = Informacoes do monitoramento"
  );

  Serial.println(
    "W = Configurar Wi-Fi (ate 5 redes)"
  );

  Serial.println(
    "Modo Wi-Fi: " +
    String(
      modoConexaoWiFi == 1
        ? "MANUAL"
        : "AUTOMATICO"
    )
  );

  Serial.println(
    "L = Limpar historico local"
  );

  Serial.println(
    "============================================"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // ----------------------------------------------------------
  // MODO TEMPORARIO APOS LIMPEZA
  // ----------------------------------------------------------

  if (modoAposLimpeza) {

    // Nao le sensores.
    // Nao envia dados.
    // Nao conecta na nuvem.

    delay(1000);

    return;
  }

  // ----------------------------------------------------------
  // SERVIDOR
  // ----------------------------------------------------------

  servidor.handleClient();

  // ----------------------------------------------------------
  // PROCESSAR ALTERACAO DE CONEXAO WI-FI
  // ----------------------------------------------------------

  processarConexaoWiFiSolicitada();

  // ----------------------------------------------------------
  // LEITURA DOS SENSORES
  // ----------------------------------------------------------

  if (

    millis() -
    ultimaLeituraSensor >=
    INTERVALO_SENSOR

  ) {

    ultimaLeituraSensor =
      millis();

    lerSensores();
  }

  // ----------------------------------------------------------
  // REGISTRO
  // ----------------------------------------------------------

  if (

    millis() -
    ultimoRegistro >=
    INTERVALO_REGISTRO

  ) {

    ultimoRegistro =
      millis();

    mostrarMonitoramento();

    registrarMonitoramento();
  }

  // ----------------------------------------------------------
  // COMANDOS SERIAL
  // ----------------------------------------------------------

  if (
    Serial.available()
  ) {

    char comando =
      Serial.read();

    if (
      comando == 'H' ||
      comando == 'h'
    ) {

      Serial.println();

      Serial.println(
        "[HISTORICO] Registros locais:"
      );

      Serial.println(
        quantidadeRegistros
      );
    }

    if (
      comando == 'P' ||
      comando == 'p'
    ) {

      mostrarPerfis();
    }

    if (
      comando == 'C' ||
      comando == 'c'
    ) {

      mostrarInformacoes();
    }

    if (
      comando == 'L' ||
      comando == 'l'
    ) {

      limparHistorico();
    }

    if (
      comando == 'W' ||
      comando == 'w'
    ) {

      iniciarPortalWiFi();
    }
  }
}