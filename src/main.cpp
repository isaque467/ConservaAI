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

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    String chaveSSID = "ssid" + String(i + 1);
    String chaveSenha = "senha" + String(i + 1);

    redesWiFi[i].ssid =
      preferencias.getString(chaveSSID.c_str(), "");

    redesWiFi[i].senha =
      preferencias.getString(chaveSenha.c_str(), "");

    if (redesWiFi[i].ssid.length() > 0) {
      wifiConfigurado = true;
    }
  }

  preferencias.end();

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
}

// ============================================================

bool conectarWiFi() {

  if (!wifiConfigurado) {

    Serial.println("[WIFI] Nenhuma rede cadastrada.");
    return false;
  }

  Serial.println();
  Serial.println("============================================");
  Serial.println("       TENTANDO REDES WI-FI SALVAS");
  Serial.println("============================================");

  WiFi.mode(WIFI_STA);

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    if (redesWiFi[i].ssid.length() == 0) {
      continue;
    }

    Serial.print("[WIFI] Tentando rede ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(redesWiFi[i].ssid);

    WiFi.disconnect();
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

    if (WiFi.status() == WL_CONNECTED) {

      redeSalva = redesWiFi[i].ssid;
      senhaSalva = redesWiFi[i].senha;

      nuvemAtiva = true;

      Serial.println("[WIFI] Conectado com sucesso.");

      Serial.print("[WIFI] Rede utilizada: ");
      Serial.println(redeSalva);

      Serial.print("[WIFI] Endereco IP: ");
      Serial.println(WiFi.localIP());

      Serial.println("============================================");

      return true;
    }

    Serial.println("[WIFI] Falha nesta rede.");
  }

  WiFi.disconnect(true);

  nuvemAtiva = false;

  Serial.println("[WIFI] Nenhuma rede salva conseguiu conexao.");
  Serial.println("[WIFI] Use o comando W para abrir o portal.");

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

String htmlEscapar(String texto) {

  texto.replace("&", "&amp;");
  texto.replace("<", "&lt;");
  texto.replace(">", "&gt;");
  texto.replace("\"", "&quot;");

  return texto;
}

// ============================================================

void paginaWiFi() {

  String html =
    "<!DOCTYPE html>"
    "<html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ConservaAI - Wi-Fi</title>"
    "</head>"
    "<body style='font-family:Arial;padding:20px;max-width:700px;margin:auto'>"
    "<h1>ConservaAI</h1>"
    "<h2>Redes Wi-Fi cadastradas</h2>"
    "<p>O ESP32 pode guardar ate 5 redes. "
    "Ele tentara conectar automaticamente na ordem abaixo.</p>"
    "<form action='/salvarwifi' method='GET'>";

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    html +=
      "<hr><h3>Rede " + String(i + 1) + "</h3>"
      "<label>Nome da rede (SSID):</label><br>"
      "<input name='ssid" + String(i + 1) +
      "' value='" + htmlEscapar(redesWiFi[i].ssid) +
      "' style='width:100%;padding:10px;box-sizing:border-box'><br><br>"
      "<label>Senha:</label><br>"
      "<input name='senha" + String(i + 1) +
      "' type='password' value='" + htmlEscapar(redesWiFi[i].senha) +
      "' style='width:100%;padding:10px;box-sizing:border-box'><br><br>";
  }

  html +=
    "<button type='submit' style='padding:12px 20px'>Salvar redes</button>"
    "</form><hr>"
    "<p><b>Importante:</b> deixe SSID e senha vazios "
    "para manter uma posicao sem rede.</p>"
    "</body></html>";

  servidor.send(200, "text/html", html);
}

// ============================================================

void salvarWiFiWeb() {

  preferencias.begin("wifi", false);

  int redesSalvasAgora = 0;

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    String chaveSSID = "ssid" + String(i + 1);
    String chaveSenha = "senha" + String(i + 1);

    String ssid = servidor.arg("ssid" + String(i + 1));
    String senha = servidor.arg("senha" + String(i + 1));

    ssid.trim();

    redesWiFi[i].ssid = ssid;
    redesWiFi[i].senha = senha;

    preferencias.putString(chaveSSID.c_str(), ssid);
    preferencias.putString(chaveSenha.c_str(), senha);

    if (ssid.length() > 0) {
      redesSalvasAgora++;
    }
  }

  preferencias.end();

  wifiConfigurado = redesSalvasAgora > 0;

  redeSalva = "";
  senhaSalva = "";

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    if (redesWiFi[i].ssid.length() > 0) {
      redeSalva = redesWiFi[i].ssid;
      senhaSalva = redesWiFi[i].senha;
      break;
    }
  }

  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ConservaAI - Wi-Fi</title>"
    "</head><body style='font-family:Arial;padding:20px;max-width:700px;margin:auto'>"
    "<h1>Wi-Fi salvo</h1><p>" +
    String(redesSalvasAgora) +
    " rede(s) cadastrada(s).</p>"
    "<p>Reinicie o ESP32 para que ele tente conectar "
    "automaticamente as redes na ordem cadastrada.</p>"
    "<a href='/'>Voltar para configuracao</a>"
    "</body></html>";

  servidor.send(200, "text/html", html);

  Serial.println();
  Serial.println("[WIFI] Configuracao atualizada pelo portal.");
  Serial.print("[WIFI] Redes cadastradas: ");
  Serial.println(redesSalvasAgora);

  for (int i = 0; i < MAX_REDES_WIFI; i++) {

    Serial.print("  Rede ");
    Serial.print(i + 1);
    Serial.print(": ");

    if (redesWiFi[i].ssid.length() > 0) {
      Serial.println(redesWiFi[i].ssid);
    } else {
      Serial.println("(vazia)");
    }
  }
}

// ============================================================

void iniciarPortalWiFi() {

  portalAtivo = true;

  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_AP);

  WiFi.softAP(AP_NOME, AP_SENHA);

  IPAddress ip = WiFi.softAPIP();

  servidor.on("/", HTTP_GET, paginaWiFi);
  servidor.on("/salvarwifi", HTTP_GET, salvarWiFiWeb);

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

  conectarWiFi();

  // ----------------------------------------------------------
  // NTP
  // ----------------------------------------------------------

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    sincronizarHorario();
  }

  // ----------------------------------------------------------
  // SERVIDOR
  // ----------------------------------------------------------

  configurarRotas();

  servidor.begin();

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