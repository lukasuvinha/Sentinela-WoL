// Template publicavel de credenciais.
// Copie este arquivo para src/secrets.h e preencha com os valores reais.
// O src/secrets.h esta no .gitignore e NUNCA deve ser commitado.
//
//   cp src/secrets.example.h src/secrets.h
//   $EDITOR src/secrets.h

#pragma once

// Os valores abaixo sao propositalmente os placeholders que o
// static_assert do main.cpp rejeita. Enquanto estiverem aqui, o build
// falha com uma mensagem clara em vez de gerar um firmware que nao
// conecta. Antes ficavam como string vazia, e a compilacao passava.

// SSID da rede Wi-Fi 2.4 GHz (o ESP32 nao fala 5 GHz).
#define SECRET_WIFI_SSID     "PREENCHER_SSID_AQUI"

// Senha da rede Wi-Fi.
#define SECRET_WIFI_PASSWORD "PREENCHER_SENHA_AQUI"
