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

String redeSalva = "";
String senhaSalva = "";

bool wifiConfigurado = false;
bool portalAtivo = false;


// ============================================================
// NTP
// ============================================================

const char* SERVIDOR_NTP = "pool.ntp.org";

const long GMT_OFFSET = -3 * 3600;
const int DST_OFFSET = 0;

bool horarioSincronizado = false;


// ============================================================
// DADOS DO SISTEMA
// ============================================================

float temperaturaAtual = NAN;
float umidadeAtual = NAN;

bool poucaLuz = true;

unsigned long ultimaLeituraSensor = 0;
unsigned long ultimoRegistro = 0;

const unsigned long INTERVALO_SENSOR = 2000;
const unsigned long INTERVALO_REGISTRO = 10000;


// ============================================================
// CONTEXTO
// ============================================================

String loteAtual = "LOTE-NAO-DEFINIDO";
String localAtual = "LOCAL-NAO-DEFINIDO";
String usuarioAtual = "USUARIO-NAO-DEFINIDO";


// ============================================================
// NUVEM
// ============================================================

bool nuvemAtiva = false;

String ultimoEnvioNuvem =
  "Aguardando primeiro envio.";


// ============================================================
// PRODUTOS
// ============================================================

enum Produto {
  ALFACE,
  TOMATE,
  CEBOLA
};

Produto produtoAtual = ALFACE;


// ============================================================
// PERFIS
// ============================================================

struct PerfilCultura {

  String nome;

  String estagio;

  float temperaturaMin;
  float temperaturaMax;

  float umidadeMin;
  float umidadeMax;

};


PerfilCultura perfilAlface = {

  "ALFACE",

  "NAO SE APLICA",

  5.0,
  10.0,

  85.0,
  95.0

};


PerfilCultura perfilTomate = {

  "TOMATE",

  "NAO DEFINIDO",

  10.0,
  15.0,

  85.0,
  95.0

};


PerfilCultura perfilCebola = {

  "CEBOLA",

  "NAO SE APLICA",

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
    "%d/%m/%Y %H:%M:%S",
    &tempo
  );

  return String(buffer);

}


String gerarIdMedicao() {

  unsigned long agora = millis();

  return "MED-" + String(agora);

}


String urlEncode(String texto) {

  String resultado = "";

  const char* hex = "0123456789ABCDEF";

  for (unsigned int i = 0; i < texto.length(); i++) {

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

      resultado += hex[(c >> 4) & 0x0F];

      resultado += hex[c & 0x0F];

    }

  }

  return resultado;

}


// ============================================================
// PERFIL
// ============================================================

PerfilCultura obterPerfil(Produto produto) {

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

    motivo += "Temperatura muito baixa";

  }

  if (temperaturaAlta) {

    motivo += "Temperatura muito alta";

  }

  if (umidadeBaixa) {

    if (motivo.length() > 0) {

      motivo += " | ";

    }

    motivo += "Umidade muito baixa";

  }

  if (umidadeAlta) {

    if (motivo.length() > 0) {

      motivo += " | ";

    }

    motivo += "Umidade muito alta";

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


void salvarRegistroLocal(Registro registro) {

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
    String(registro.temperatura, 1) + ";" +
    String(registro.umidade, 1) + ";" +
    registro.luz + ";" +
    registro.status + ";" +
    registro.motivo
  );


  arquivo.close();

}


void carregarHistorico() {

  quantidadeRegistros = 0;


  if (!LittleFS.exists("/historico.txt")) {

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


    // O historico antigo pode existir.
    // Neste ponto apenas informamos a quantidade.
    quantidadeRegistros++;

  }


  arquivo.close();


  Serial.print(
    "[LITTLEFS] Registros recuperados: "
  );

  Serial.println(quantidadeRegistros);

}


void limparHistorico() {

  if (LittleFS.exists("/historico.txt")) {

    LittleFS.remove("/historico.txt");

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

  preferencias.begin(
    "wifi",
    true
  );

  redeSalva =
    preferencias.getString(
      "ssid",
      ""
    );

  senhaSalva =
    preferencias.getString(
      "senha",
      ""
    );

  preferencias.end();


  if (redeSalva.length() > 0) {

    wifiConfigurado = true;

  }

}


bool conectarWiFi() {

  if (!wifiConfigurado) {

    return false;

  }


  Serial.println();
  Serial.println(
    "============================================"
  );
  Serial.println(
    "              CONECTANDO WI-FI"
  );
  Serial.println(
    "============================================"
  );

  Serial.print("Rede: ");
  Serial.println(redeSalva);


  WiFi.mode(WIFI_STA);

  WiFi.begin(
    redeSalva.c_str(),
    senhaSalva.c_str()
  );


  int tentativas = 0;


  while (
    WiFi.status() != WL_CONNECTED &&
    tentativas < 30
  ) {

    delay(500);

    Serial.print(".");

    tentativas++;

  }


  Serial.println();


  if (
    WiFi.status() == WL_CONNECTED
  ) {

    nuvemAtiva = true;

    Serial.println(
      "[WIFI] Conectado com sucesso."
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
    "[WIFI] Falha ao conectar."
  );

  return false;

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


  if (getLocalTime(&tempo)) {

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
// ENVIO DE MEDICAO PARA A NUVEM
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
    String(URL_CONSERVAAI_NUVEM) +
    "?acao=registrar_medicao" +

    "&id_medicao=" +
    urlEncode(idMedicao) +

    "&data_hora_medicao=" +
    urlEncode(dataHora) +

    "&dispositivo=" +
    urlEncode(ID_DISPOSITIVO) +

    "&usuario=" +
    urlEncode(usuarioAtual) +

    "&local=" +
    urlEncode(localAtual) +

    "&lote=" +
    urlEncode(loteAtual) +

    "&temperatura=" +
    urlEncode(String(temperatura, 1)) +

    "&umidade=" +
    urlEncode(String(umidade, 1)) +

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

  Serial.println(codigo);


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
    String(URL_CONSERVAAI_NUVEM) +
    "?acao=registrar_avaliacao" +

    "&id_medicao=" +
    urlEncode(idMedicao) +

    "&produto=" +
    urlEncode(perfil.nome) +

    "&estagio=" +
    urlEncode(perfil.estagio) +

    "&status=" +
    urlEncode(status) +

    "&motivo=" +
    urlEncode(motivo) +

    "&perfil_versao=" +
    urlEncode(perfil.nome + "-V1");


  Serial.print(
    "[NUVEM] Enviando avaliacao: "
  );

  Serial.println(perfil.nome);


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

  Serial.println(codigo);


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
// REGISTRO COMPLETO
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
  // 1. SALVA A MEDICAO LOCAL
  // ----------------------------------------------------------

  // A medicao fisica e salva uma unica vez.
  Registro base;

  base.dataHora = dataHora;

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
    "NAO SE APLICA";

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


  salvarRegistroLocal(base);


  Serial.println();

  Serial.println(
    "[HISTORICO] Nova medicao salva."
  );


  // ----------------------------------------------------------
  // 2. ENVIA MEDICAO PARA NUVEM
  // ----------------------------------------------------------

  if (nuvemAtiva) {

    bool medicaoEnviada =
      enviarMedicaoNuvem(
        idMedicao,
        dataHora,
        temperaturaAtual,
        umidadeAtual,
        luz
      );


    if (!medicaoEnviada) {

      Serial.println(
        "[NUVEM] Medicao nao enviada."
      );

    }

  }


  // ----------------------------------------------------------
  // 3. AVALIA CADA CULTURA
  // ----------------------------------------------------------

  PerfilCultura perfis[3] = {

    perfilAlface,
    perfilTomate,
    perfilCebola

  };


  for (int i = 0; i < 3; i++) {

    String motivo;

    String status =
      obterStatus(
        perfis[i],
        temperaturaAtual,
        umidadeAtual,
        motivo
      );


    Registro avaliacao;

    avaliacao.dataHora =
      dataHora;

    avaliacao.dispositivo =
      ID_DISPOSITIVO;

    avaliacao.usuario =
      usuarioAtual;

    avaliacao.local =
      localAtual;

    avaliacao.lote =
      loteAtual;

    avaliacao.produto =
      perfis[i].nome;

    avaliacao.estagio =
      perfis[i].estagio;

    avaliacao.temperatura =
      temperaturaAtual;

    avaliacao.umidade =
      umidadeAtual;

    avaliacao.luz =
      luz;

    avaliacao.status =
      status;

    avaliacao.motivo =
      motivo;


    Serial.println();

    Serial.print(
      "[AVALIACAO] "
    );

    Serial.println(
      perfis[i].nome
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

    if (motivo.length() > 0) {

      Serial.println(motivo);

    } else {

      Serial.println(
        "Parametros dentro do perfil."
      );

    }


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
    digitalRead(LDR_PIN);


  poucaLuz =
    leituraLDR == HIGH;

}


// ============================================================
// MONITORAMENTO NO SERIAL
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
    "Temperatura geral: "
  );

  Serial.print(
    temperaturaAtual,
    1
  );

  Serial.println(" C");


  Serial.print(
    "Umidade geral: "
  );

  Serial.print(
    umidadeAtual,
    1
  );

  Serial.println(" %");


  Serial.print(
    "Luz geral: "
  );

  Serial.println(luz);


  PerfilCultura perfis[3] = {

    perfilAlface,
    perfilTomate,
    perfilCebola

  };


  for (int i = 0; i < 3; i++) {

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


    if (motivo.length() > 0) {

      Serial.println(
        motivo
      );

    } else {

      Serial.println(
        "Parametros dentro do perfil."
      );

    }


    if (
      perfis[i].nome == "TOMATE"
    ) {

      Serial.print(
        "Estagio: "
      );

      Serial.println(
        perfis[i].estagio
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
// INFORMACOES
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


  for (int i = 0; i < 3; i++) {

    Serial.println();

    Serial.print(
      perfis[i].nome
    );

    Serial.println(":");


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


    Serial.print(
      "Estagio: "
    );

    Serial.println(
      perfis[i].estagio
    );

  }


  Serial.println(
    "============================================"
  );

}


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
    "O sistema realiza uma leitura geral"
  );

  Serial.println(
    "do ambiente usando DHT22 e LDR."
  );

  Serial.println();

  Serial.println(
    "A mesma medicao fisica pode ser"
  );

  Serial.println(
    "interpretada para diferentes culturas."
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

void iniciarPortalWiFi() {

  portalAtivo = true;

  WiFi.disconnect(true);

  delay(500);

  WiFi.mode(WIFI_AP);

  WiFi.softAP(
    AP_NOME,
    AP_SENHA
  );


  IPAddress ip =
    WiFi.softAPIP();


  Serial.println();

  Serial.println(
    "============================================"
  );

  Serial.println(
    "          CONFIGURACAO WI-FI"
  );

  Serial.println(
    "============================================"
  );


  Serial.print(
    "Rede: "
  );

  Serial.println(
    AP_NOME
  );


  Serial.print(
    "Endereco: "
  );

  Serial.println(ip);


  Serial.println(
    "============================================"
  );

}


void salvarCredenciais(
  String ssid,
  String senha
) {

  preferencias.begin(
    "wifi",
    false
  );


  preferencias.putString(
    "ssid",
    ssid
  );


  preferencias.putString(
    "senha",
    senha
  );


  preferencias.end();


  redeSalva = ssid;

  senhaSalva = senha;

  wifiConfigurado = true;


  Serial.println(
    "[WIFI] Credenciais salvas."
  );

}


// ============================================================
// PAINEL WEB LOCAL
// ============================================================

void paginaPrincipal() {

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
    "h1{margin-top:0}"
    ".item{padding:12px;margin:8px 0;background:#f1f1f1;border-radius:8px}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>ConservaAI</h1>"
    "<h2>Monitoramento</h2>";


  if (
    !isnan(temperaturaAtual)
  ) {

    html +=
      "<div class='item'><b>Temperatura:</b> " +
      String(temperaturaAtual, 1) +
      " C</div>";

  }


  if (
    !isnan(umidadeAtual)
  ) {

    html +=
      "<div class='item'><b>Umidade:</b> " +
      String(umidadeAtual, 1) +
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
    String(ID_DISPOSITIVO) +
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

    "<div class='item'><b>Usuario:</b> " +
    usuarioAtual +
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
    "/dados",
    HTTP_GET,
    []() {

      String resposta = "{";

      resposta +=
        "\"temperatura\":" +
        String(temperaturaAtual, 1) +
        ",";

      resposta +=
        "\"umidade\":" +
        String(umidadeAtual, 1) +
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
        String(ID_DISPOSITIVO) +
        "\"";

      resposta += "}";


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

  Serial.begin(115200);

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

  conectarWiFi();


  // ----------------------------------------------------------
  // NTP
  // ----------------------------------------------------------

  if (
    WiFi.status() == WL_CONNECTED
  ) {

    sincronizarHorario();

  }


  // ----------------------------------------------------------
  // SERVIDOR LOCAL
  // ----------------------------------------------------------

  configurarRotas();

  servidor.begin();


  if (
    WiFi.status() == WL_CONNECTED
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
    "Culturas monitoradas:"
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
    "W = Configurar Wi-Fi"
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

  servidor.handleClient();


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