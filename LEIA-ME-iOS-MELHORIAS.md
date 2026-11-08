# Refinamentos do editor, build 71206

O editor em tela cheia recebeu estas melhorias:

- **Desfazer e Refazer**, com histórico separado para retrato e paisagem.
- Cada arraste ou ajuste contínuo de tamanho/posição vale como uma única ação.
- Painel recolhível para liberar a visão da tela. Toque em um controle para
  expandir as ferramentas novamente.
- Nome da ação atribuída ao botão selecionado e indicação de botão oculto.
- Interrupção do arraste ao girar ou redimensionar a janela, preservando a
  edição da orientação anterior.

Use as setas do painel ou as opções **Desfazer/Refazer** no menu. O histórico
mantém até 40 ações em cada orientação durante a edição. **Concluir** continua
salvando os layouts; **Cancelar** descarta o rascunho. As preferências já salvas
não são alteradas até concluir.

Passaram 25 testes portáteis e a análise de sintaxe dos 19 arquivos Swift.
Os seis testes Swift, a compilação no Xcode e a validação no iPhone/iPad ainda
precisam ser executados no Mac. Esta entrega contém o código-fonte.

Abaixo está o histórico das revisões anteriores.

# Editor em tela cheia, build 71205

Abra **Controles e entrada > Personalizar controles > Organizar e remapear
controles de toque**. O editor ocupa a tela do app com fundo preto e acompanha
o tamanho e a orientação reais da janela no iPhone ou iPad.

1. Toque em um controle para selecioná-lo.
2. Arraste para a posição desejada dentro da área segura.
3. Ajuste o tamanho no controle deslizante, com resultado imediato.
4. Toque em **Concluir** para salvar e sair.

As ferramentas flutuam sobre a tela e somem durante o arraste. No menu de
opções, escolha qualquer controle, inclusive os ocultos, altere sua ação ou
ajuste a posição com precisão. Gire o aparelho para editar a outra orientação.
Os layouts de retrato e paisagem são salvos juntos ao concluir.

A área de posicionamento é compartilhada com o jogo e inclui uma margem de
16 pontos além da área segura do sistema. Em retrato, a imagem do jogo continua
na parte superior; os controles agora podem ser posicionados em toda a área
segura. As posições e tamanhos antigos são convertidos ao abrir o layout, com
limitação nas novas bordas seguras. Os arquivos anteriores são mantidos.
Cancelar descarta as alterações; uma falha de gravação mantém a edição aberta.

Passaram 25 testes portáteis e a análise de sintaxe dos 19 arquivos Swift.
Os cinco testes Swift de layout, persistência e conversão precisam rodar no Mac.
A compilação no Xcode e a conferência visual no iPhone/iPad ainda estão pendentes:

```bash
python3 -m unittest discover -s port/ios/tests -v
python3 port/ios/build_ios.py --release --jobs 4 --team SUA_EQUIPE --install
```

Pause a corrida com START antes de editar. O editor bloqueia comandos, mas
mantém o estado de pausa atual do jogo. Esta entrega contém o código-fonte.

Abaixo está o histórico das revisões anteriores.

# Posições dos controles no remapeamento, build 71204

Ao abrir o editor de toque, os botões usam as últimas posições salvas pelo
usuário em cada orientação. Sem personalização salva, usam a disposição padrão.
A prévia mantém as proporções da área de referência dos controles.

Selecionar um botão ou mudar sua ação mantém sua posição e tamanho. Para mover,
arraste o botão ou use os ajustes de posição. Botões ocultos só aparecem como
ajuda de edição quando selecionados na lista de controles.

Salvar mantém o novo layout para as próximas aberturas. Cancelar descarta o
rascunho; Restaurar este layout retorna o rascunho da orientação ao padrão.
Os 25 testes portáteis passaram. Os quatro testes Swift, a compilação no Xcode
e a conferência visual no iPhone continuam pendentes no Mac.

Abaixo está o histórico das revisões anteriores.

# Ajustes finos dos controles iOS, build 71203

Esta revisão melhora o editor de controles da versão 0.7.12:

- Ajuste de posição horizontal e vertical em passos de 1%, com os valores na tela.
- Painel com rolagem ao lado da prévia em telas horizontais mais baixas.
- Confirmação ao descartar alterações e restaurar layouts ou o controle físico.
- Trocas de comandos essenciais revelam o botão de destino, se estava oculto.
- Migração de layouts antigos preserva edições de tamanho e visibilidade.
- Cancelar um arraste limpa sua origem; mudanças de tela e atividade também
  recriam os controles de toque, exigindo um novo toque para voltar a comandar.

Abra **Controles e entrada > Personalizar controles**. No editor de toque,
**Salvar** aplica os dois layouts; **Cancelar** pergunta antes de descartar
alterações. Restaurar um layout altera apenas o rascunho daquela orientação.
O mapeamento físico continua sendo salvo imediatamente.

Passaram 25 testes portáteis e a análise de sintaxe dos 19 arquivos Swift.
Os quatro novos testes executáveis de layout e persistência foram incluídos,
mas precisam de macOS com Swift e Combine. Execute no Mac antes de compilar:

```bash
python3 -m unittest discover -s port/ios/tests -v
python3 port/ios/build_ios.py --release --jobs 4 --team SUA_EQUIPE --install
```

Esta entrega contém o código-fonte. A compilação no Xcode, a criação da IPA e
os testes da interface e dos gestos no iPhone continuam pendentes. A checagem
completa do port também depende de LLVM/WASI/WABT ausentes neste ambiente.
Pause a corrida com START antes de editar. O editor bloqueia entradas, mas
mantém o estado de pausa atual do jogo. Os detalhes estão no finding 177.

Abaixo está o histórico das revisões anteriores.

# Editor de controles iOS, build 71202

Abra **Controles e entrada > Personalizar controles** no launcher, ou
**MENU > Personalizar controles** durante o jogo.

Em **Organizar e remapear controles de toque**, selecione Retrato ou Paisagem,
arraste os controles e escolha a ação, o tamanho e a visibilidade de cada botão.
Toque em Salvar para aplicar os dois layouts. Cancelar descarta a edição.
Restaurar este layout repõe o padrão da orientação selecionada.

Em **Mapeamento do controle físico**, escolha o jogador, altere as entradas
principais e alternativas, troque os analógicos ou inverta seus eixos. Essas
alterações são aplicadas imediatamente. O jogo protege os comandos essenciais.

O novo padrão de deadzone é 5%. Valores já salvos continuam carregados.
O editor bloqueia entradas do jogador principal. Pause a corrida com START
antes de editar, pois abrir o editor mantém o estado de pausa atual do jogo.

Passaram 25 testes portáteis. A compilação no Xcode, os gestos e a gravação dos
layouts ainda precisam ser testados no iPhone. Os detalhes estão em
`docs/findings/177-ios-control-editor.md`. O histórico das revisões está abaixo.

# Atualização iOS 0.7.12

O código foi atualizado até a tag oficial v0.7.12, com correção da direção
analógica. O novo build de fontes é 71201. A zona morta do controle físico fica
em Controles e entrada, na seção de controles físicos, com padrão de 15%.
O toque mantém sua zona morta e sensibilidade próprias em retrato e paisagem.

Compile novamente o núcleo e o app no Mac, usando sua própria equipe Apple.

```bash
python3 port/ios/build_ios.py --release --jobs 4 --team SUA_EQUIPE --install
```

Substitua SUA_EQUIPE pelo identificador da sua equipe no Xcode. Para gerar
a IPA sem assinatura, use `--unsigned` no lugar de `--team SUA_EQUIPE --install`.
A instalação e a jogabilidade desta revisão ainda precisam ser validadas
no iPhone. Abaixo está o histórico das revisões anteriores.

# Downhill Revived 0.7.9 para iOS

O build 70901 integra a tag oficial v0.7.9. A atualização compartilha a coleta
de itens entre os participantes de corridas online e corrige a câmera do modo
espectador. A escala interna pode chegar a 6x, conforme o limite da GPU.

## AirPlay e HDMI

No iPhone, abra a Central de Controle, toque em **Espelhamento de Tela** e
selecione a TV ou o Apple TV. O launcher e seus menus aparecem na TV enquanto
você navega pelo iPhone. Ao iniciar o jogo, a TV passa a mostrar a partida.
Os controles e o menu de pausa ficam no iPhone. O mesmo fluxo funciona com HDMI.

A interface do port segue o idioma preferido do iOS. Há traduções em inglês,
português do Brasil, espanhol, francês, alemão, italiano, japonês e chinês
simplificado. O inglês é o padrão quando nenhum idioma preferido tem tradução.
A localização abrange o launcher e o menu de ajustes durante o jogo.

## Ajustes gráficos do port

O build 70704 corrige a atualização de câmera quando a proporção muda, aplica a
compensação horizontal a mais três caminhos de sprites e usa até 16:9 na tenda.
A corrida volta ao Wide selecionado ao sair da tenda.

Na TV, a imagem fica em 16:9 e o jogo começa com escala interna 3x. A meta é
120 FPS quando o iOS informa suporte a essa taxa na conexão ativa e 60 FPS nas
demais conexões. Em economia de energia ou sob aquecimento elevado, a meta é
60 FPS. O contador mostra a cadência observada separadamente.

Abra **MENU > Configurações** para mudar o perfil, a escala interna, a
resolução de saída e o filtro durante a sessão. A proporção e a meta de FPS
podem ser escolhidas no iPhone. Na TV elas são automáticas. A escala escolhida
para a TV vale até desconectar. O modo de saída acima de 1080p e a sincronização
vertical são definidos antes de abrir o jogo.

## Teste no aparelho

1. Em Wide (Hor+), entre na tenda e alterne entre roupa e bicicleta. Confira o
   personagem, a bicicleta e a volta ao Wide ao sair.
2. Durante uma corrida, confira os power-ups em 4:3, 16:9 e Wide (Hor+).
3. Conecte a TV por AirPlay antes de abrir o jogo. Navegue pelo launcher,
   inicie uma partida e confirme que o menu de pausa fica no iPhone. Teste
   desconectar e reconectar durante a partida e repita via HDMI. Confira
   a escala inicial 3x, o formato 16:9 e o FPS medido.
4. Alterne entre 1x e 3x, 540p e 1080p, e entre os filtros suave e nítido.
   A troca de escala interna recria superfícies e pode piscar por um quadro.

## Correções adicionais da revisão 2

- O analógico trata zona morta zero, entradas inválidas e valores extremos sem produzir NaN. As configurações são inicializadas antes do launcher e validadas ao carregar o layout.
- Comandos de toque são liberados na mudança de orientação, ao abrir o menu e ao perder atividade, evitando comandos presos. O estado visual dos controles também é limpo.
- O ciclo de vida é acompanhado por `scenePhase`, além do AppDelegate. O renderer evita novas aquisições quando o app está inativo e mantém o timeout de drawables habilitado no iOS. Ainda é necessário testar essas transições no aparelho.
- Largura e altura da saída são publicadas juntas, evitando combinar dimensões de duas orientações. Consultas ao UIScreen ficam na thread principal. A recuperação da resolução após aquecimento aguarda 30 segundos para reduzir oscilações.
- Importação, exclusão e início do jogo passam por uma trava de operação. A ISO anterior é substituída por `rename` sem ser apagada antes. URLs locais são aceitas mesmo quando não precisam de acesso com security scope.
- O Xcode cuida da assinatura com a equipe selecionada. O script não escolhe um certificado arbitrário nem altera os assets depois de assinar. Assinatura e provisioning profile são verificados antes de empacotar uma compilação assinada.
- A IPA é criada diretamente, mantendo permissões de execução e links simbólicos. Um erro de gravação preserva a IPA anterior. A revisão atual se identifica como `0.7.9 (iOS r1)`, build `70901`.

## Compilar no seu Mac

Extraia este ZIP em uma pasta nova. Copie apenas o seu `iso/SCUS_971.77` da
pasta de compilação anterior ou extraia esse arquivo da sua ISO americana.
O build gera os arquivos auxiliares a partir dele. Instale as dependências
Python com `python3 -m pip install -r requirements.txt` se necessário.

Na pasta do projeto, com o mesmo ambiente de ferramentas que você usou para a versão original:

```bash
python3 port/ios/build_ios.py --release --jobs 4 --install
```

Se precisar selecionar outra equipe Apple, acrescente `--team SEU_TEAM_ID` ou configure `DHD_APPLE_TEAM`. O identificador contém dez letras maiúsculas ou dígitos. O padrão da equipe do projeto enviado foi mantido.

Com mais de um dispositivo disponível, selecione o identificador de um deles com `--device IDENTIFICADOR`. O script não escolhe arbitrariamente entre vários aparelhos.

Quando você quiser permitir que o Xcode atualize certificados e perfis na sua conta Apple, use `--allow-provisioning-updates`. Sem essa opção, o script usa os recursos de assinatura já configurados. Se faltarem, configure a equipe na aba Signing & Capabilities do Xcode.

`--unsigned` permite verificar a compilação sem assinatura. O resultado não é instalável pelo script; essa opção não pode ser combinada com `--install`.

O script agora recompila o núcleo wasm, o host ARM64 e o renderer Rust antes de gerar o projeto e o aplicativo. Se `assets/vutext.bin` não existir, ele usa `iso/SCUS_971.77` para regenerar os arquivos com `tools/split.py`. As dependências Python de `requirements.txt` precisam estar instaladas.

São necessários macOS, Xcode com SDK iPhoneOS, xcodegen e o ambiente LLVM, WASI, WABT, SIMDe, CMake, Ninja e Rust usado no projeto original. As variáveis de ambiente do seu setup anterior continuam necessárias quando as ferramentas não estão nos caminhos padrão.

Sem instalar no telefone:

```bash
python3 port/ios/build_ios.py --release --jobs 4
```

Somente recompilar o núcleo e o renderer:

```bash
python3 port/ios/build_ios.py --core-only --jobs 4
```

`--wasm CAMINHO` reutiliza deliberadamente um módulo já compilado. Não use o wasm da versão antiga para testar a correção do relógio. `--bundle-only` prepara fontes para o Xcode, sem atualizar o núcleo.

O projeto gerado fica em `build/ios/project`. A compilação usa `build/ios/DerivedData`, evitando selecionar outro aplicativo antigo na pasta global do Xcode. A IPA fica em `build/ios/DownhillRevived.ipa`.

## O que mudou

1. Relógio baseado no tempo decorrido, importado do upstream `01f8d2bd79908f5eadd2d784de6edbea32ade1a2`. O conjunto inclui física, animações, áudio, input, contadores e sincronização online. Mantém o tempo real até aproximadamente 30 atualizações por segundo; abaixo disso, o limite de segurança da simulação ainda permite câmera lenta. Não aumenta a capacidade de renderização do telefone.
2. Configuração de buffers do wgpu corrigida. A latência de apresentação usa 2 quando há espera pela tela, permitindo três buffers. O renderer escolhe o padrão conforme o modo de apresentação e respeita overrides de diagnóstico.
3. O perfil Performance passa a usar 1x/60 FPS. A primeira abertura desta revisão migra as preferências gráficas para esse perfil. As demais preferências e os saves permanecem como estavam.
4. A resolução final de saída passa a ter limite no lado menor: 720 pixels em Performance, 900 em Balanced e 1080 em Quality. Isso mantém a proporção do viewport. A resolução interna das superfícies do jogo é uma configuração separada.
5. O estado térmico e o Modo Pouca Energia são consultados uma vez por segundo. Quando necessário, a resolução de saída cai para até 720 pixels no lado menor. Em Performance ela já está nesse limite. O app também reduz a solicitação de atualização da tela quando está quente ou em economia de energia.
6. O modo Auto interno escolhe 1x ou 2x conforme as condições do aparelho. A escala pode mudar durante o jogo.
7. A opção de 120 FPS fica disponível somente quando há suporte ProMotion. Um CADisplayLink solicita a cadência da tela; ele não cria um segundo loop de renderização. A solicitação não garante que iOS, GPU ou simulação sustentem essa taxa.
8. A telemetria usa intervalos entre chamadas de apresentação concluídas. Mostra média e p99 de uma janela de até 240 intervalos. Não devolve a meta configurada como se fosse FPS medido. Esses intervalos não equivalem a timestamps de execução da GPU nem a confirmação de exibição física de cada quadro.
9. O ciclo de vida passa a interromper o envio de novos quadros quando o aplicativo perde atividade e recria a superfície ao voltar. O estado de execução usa operações atômicas e bloqueia inicialização duplicada.
10. As preferências deixam de regravar configurações do motor que não mudaram, reduzindo escritas e `fsync` desnecessários no menu.

## Validação realizada

```bash
python3 -m unittest discover -s port/ios/tests -v
```

Passaram 22 testes neste ambiente Linux. Eles incluem a compilação e execução das estatísticas e da matemática do analógico em C, acesso concorrente aos contadores, reset após pausa, verificação de 40.401 pares de entradas do analógico, empacotamento com permissões e links simbólicos e preservação da IPA anterior em caso de erro. Os testes de assinatura, seleção do dispositivo e ordem de build usam subprocessos simulados; não executam Xcode ou codesign reais.

O teste suplementar do relógio compila o `fps.c` de produção com a suíte `test_fps.c` do upstream e memória de teste. Passaram 23 execuções, incluindo a troca 120 → 60 → 120 e as combinações de taxa e modo: 30, 45, 60, 61, 75, 90, 100, 120, 144, 165 e 240 FPS, em modo determinístico e com tempo decorrido. Incluem passos, animação, física, jitter, interrupções e tempo do áudio. É uma execução nativa Linux, não a validação completa wasm32 do upstream.

Também foram conferidas as âncoras de patches em 242 unidades de código e 14 cenários portáteis de replay e fantasma, em sete taxas. As fontes de `src/` e `include/` não foram alteradas.

Não foram executados Xcode, compilação Swift/Objective-C, compilação Rust para iOS, validação Metal, instalação, assinatura real ou benchmark no aparelho. Também foram verificados sintaticamente os caminhos C compartilhados em `present.c` e `gpu_rust.c` no Linux. Isso não substitui a compilação dos caminhos Apple. Ainda é necessário conferir esses pontos no seu Mac e iPhone. Não há promessa de 60 ou 120 FPS.

## Teste no iPhone

1. Compile e instale em Release. Comece em Performance, 1x e 60 FPS.
2. Rode uma corrida por pelo menos um minuto e compare o tempo do jogo com o tempo real.
3. Observe os FPS medidos. Depois de algumas voltas, verifique aquecimento, áudio, rotação e retorno do aplicativo após bloquear a tela.
4. Para testar outra resolução interna, abra MENU > Configurações durante a sessão.
5. Confira as linhas `perf:` e `clock:` do `dhd.log`. Elas ajudam a separar limitação de CPU/VU, GPU e espera de apresentação. Quando iniciado pelo Xcode, o log pode aparecer no console em vez de um arquivo.

Para jogar online, use a versão 0.7.9 em todos os participantes.

## Fontes

- Upstream: https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.9
- Relógio e buffers: `docs/findings/139-delta-time-clock.md`
- ProMotion: https://developer.apple.com/documentation/quartzcore/optimizing-iphone-and-ipad-apps-to-support-promotion-displays
- Detalhes desta revisão: `docs/findings/168-ios-performance-review.md`
- Correções da revisão 2: `docs/findings/169-ios-stability-review.md`

- Refinamentos da revisão 3: `docs/findings/170-ios-tv-touch-refinements.md`
