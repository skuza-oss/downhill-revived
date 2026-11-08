# 149. Localização da interface iOS

Status: partly superseded by 176 (0.7.12 integration and analog input)
Checked: 2026-11-07

A revisão 0.7.8 (iOS r2), build 70802, inclui oito catálogos
`Localizable.strings`. O bundle usa inglês como idioma de desenvolvimento.
O iOS seleciona o melhor idioma compatível da lista de preferências.
Os idiomas disponíveis são inglês, português do Brasil, espanhol, francês,
alemão, italiano, japonês e chinês simplificado.

A localização cobre o launcher, o menu de pausa do port, os gráficos,
os controles, o gerenciamento de disco, as configurações do jogo, a tela
online e os créditos. Os menus do jogo de PS2 são fornecidos pela imagem
de disco. As preferências gravam os mesmos identificadores e valores.

Os testes portáteis conferem a cópia dos recursos para o projeto, o idioma
de desenvolvimento, as chaves e os argumentos de formatação. A compilação
no Xcode e a revisão visual nos aparelhos ainda precisam ser feitas.
