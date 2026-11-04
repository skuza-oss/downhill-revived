# Como gerar o IPA com ChatGPT, Gemini ou Claude

> Este tutorial foi escrito para o fork nativo de iOS do **Downhill Revived 0.7.9**. A IA ajuda a programar, inspecionar logs e resolver erros; a compilação de verdade depende das ferramentas da Apple e das dependências do projeto.

[Voltar ao README](../README.pt-BR.md) | [English](BUILD_IPA_WITH_AI.md)

## 1. O que a IA faz e o que ela não faz

ChatGPT, Gemini ou Claude podem analisar os arquivos do repositório, sugerir correções, orientar a instalação das dependências, interpretar mensagens do Xcode, ajustar arquivos Swift/Objective-C/C/Rust e ajudar a comparar o código com o upstream. **Eles não geram um aplicativo iOS legítimo apenas com uma conversa**, não fornecem credenciais Apple, não assinam o aplicativo por conta própria e não substituem o disco original do jogo.

O script incluído no código-fonte é `port/ios/build_ios.py`. Ele prepara o core, chama as ferramentas do Xcode, monta o app e gera `build/ios/DownhillRevived.ipa`.

## 2. Requisitos da máquina

- **Mac com macOS e Xcode completo** compatível com o SDK do iOS 17 ou mais recente. Execute `xcode-select -p` e `xcrun --sdk iphoneos --show-sdk-path` para confirmar a instalação.
- **Git, Python 3, XcodeGen, Rust e Cargo**, LLVM/Clang e as demais dependências do projeto.
- **wasi-sdk (linha 34), WABT/wasm2c, SIMDe** e ferramentas detalhadas em [`docs/BUILDING.md`](BUILDING.md) do repositório original. Algumas etapas precisam de configuração específica conforme a CPU e o ambiente.
- **Seu próprio disco de Downhill Domination USA, SCUS-97177**, convertido legalmente em `.iso`. Para gerar o core, a extração local do executável `SCUS_971.77` também é necessária.
- Para assinar e instalar: conta Apple adicionada ao Xcode, um **Team ID seu** e provisionamento compatível com o iPhone/iPad de destino.

O ZIP de fontes fornecido para esta versão não inclui `iso/SCUS_971.77`, `assets/vutext.bin`, `build/wasm/dhd_core.wasm` nem as bibliotecas de compilação. **Portanto, ele não compila integralmente em uma máquina nova sem preparar os dados e as ferramentas.** Não adicione ISO, arquivos extraídos do jogo ou certificados ao GitHub.

## 3. Preparar o projeto

No Mac, instale e configure o Xcode. As instruções abaixo são uma referência inicial, não uma instalação automática de todas as dependências:

```bash
# Confira as ferramentas Apple
xcodebuild -version
xcrun --sdk iphoneos --show-sdk-path

# Se já usa Homebrew
brew install git python xcodegen rustup-init

# Confirme que as ferramentas estão disponíveis
python3 --version
xcodegen --version
rustc --version
cargo --version
```

Se `rustc` ou `cargo` não estiverem disponíveis, finalize a instalação do Rust conforme `https://rustup.rs`. Consulte os caminhos de `DHD_DEVCACHES`, `WASI_SYSROOT`, `WABT_DIR` e `SIMDE_DIR` na documentação do projeto original. O uso de ambientes Python virtuais pode ser necessário, dependendo do macOS.

Em seu fork:

```bash
git clone https://github.com/SEU-USUARIO/downhill-revived.git
cd downhill-revived

# Instale as dependências Python no ambiente apropriado
python3 -m pip install -r requirements.txt
```

O projeto de iOS está em `port/ios/`, e as fontes compartilhadas do jogo continuam nas pastas originais. Não substitua todo o repositório por arquivos gerados pela IA.

## 4. Preparar os dados de compilação do seu próprio disco

Siga o fluxo de **[`docs/BUILDING.md`](BUILDING.md)** do upstream: extraia de sua `.iso` original o executável **`SCUS_971.77`** para `iso/SCUS_971.77` e execute a preparação dos assets com `tools/split.py`.

No upstream, o **SHA-1 do executável extraído**, e não o SHA da ISO inteira, é:

```text
4e25a222ad8a475b5d26c2e928e90b5f2b7075eb
```

Verificação do executável em macOS:

```bash
shasum -a 1 iso/SCUS_971.77
python3 tools/split.py
```

Se o hash for diferente, confira se o disco é a edição americana correta e não modificada. **Não utilize arquivos compartilhados por terceiros e não coloque os dados extraídos no Git.**

## 5. Gerar um IPA sem assinatura

Para obter um artefato de inspeção, depois de todas as dependências preparadas:

```bash
python3 port/ios/build_ios.py --unsigned
```

O arquivo esperado será:

```text
build/ios/DownhillRevived.ipa
```

Esse arquivo é gerado com `CODE_SIGNING_ALLOWED=NO`, conforme o script. **Não é um IPA instalável diretamente no iPhone.** Pode servir para revisão da estrutura e conferência de conteúdo.

Para apenas gerar o projeto do Xcode (sem compilar o core):

```bash
python3 port/ios/build_ios.py --bundle-only
```

Esse modo gera a estrutura do projeto, mas não equivale a um aplicativo funcional se a biblioteca nativa do game estiver ausente.

## 6. Gerar um IPA assinado pelo Xcode

Descubra seu **Team ID** no portal Apple Developer ou nas configurações do Xcode. Nunca use o identificador padrão que possa aparecer no script, pois ele pode pertencer a outra pessoa ou organização.

```bash
python3 port/ios/build_ios.py \
  --team ABCDEFGHIJ \
  --allow-provisioning-updates
```

Para instalar no iPhone já emparelhado e configurado para desenvolvimento, o script também oferece:

```bash
python3 port/ios/build_ios.py \
  --team ABCDEFGHIJ \
  --allow-provisioning-updates \
  --install
```

Substitua `ABCDEFGHIJ` pelo **seu identificador válido de 10 caracteres**. Se necessário, configure um identificador de pacote (`PRODUCT_BUNDLE_IDENTIFIER`) exclusivo, de acordo com o seu perfil de provisionamento e sem apagar créditos do projeto original. O Xcode precisa conseguir emitir/selecionar assinatura e perfil compatíveis com o aparelho.

**Atenção:** uma assinatura de desenvolvimento pode se aplicar somente a dispositivos registrados e possuir limitações de validade ou distribuição. Não publique uma versão como "instalável por qualquer pessoa" sem ter testado e confirmado o método de assinatura e instalação.

## 7. Validar o IPA gerado

```bash
# Integridade
shasum -a 256 build/ios/DownhillRevived.ipa

# Conteúdo do Info.plist
unzip -p build/ios/DownhillRevived.ipa \
  'Payload/Downhill Revived.app/Info.plist' > /tmp/downhill-info.plist
plutil -p /tmp/downhill-info.plist
```

Confirme `MinimumOSVersion` = `17.0`, `CFBundleShortVersionString` = `0.7.9`, arquitetura ARM64, compatibilidade Metal e a presença de perfil de provisionamento **se a saída for assinada**.

Você também pode executar o verificador deste material:

```bash
python3 scripts/verify_release.py build/ios/DownhillRevived.ipa
```

O SHA-256 publicado na primeira versão deste fork (`4181572b...`) refere-se **exclusivamente** ao IPA não assinado, build 70902, recebido para a publicação. Uma nova compilação ou assinatura não deve reutilizar aquele checksum.

## 8. Prompt pronto para ChatGPT, Gemini ou Claude

Copie e cole, anexando o projeto ou disponibilizando os arquivos de código relevantes:

> Você é um engenheiro experiente em portabilidade de jogos para iOS. Estou trabalhando em um fork GPL-3.0-or-later do projeto https://github.com/skuza-oss/downhill-revived, baseado na versão 0.7.9. Minha implementação iOS está em `port/ios/` e usa SwiftUI, Objective-C, Metal, núcleo C/wasm2c e renderização em Rust. Analise primeiro `port/ios/build_ios.py`, `port/ios/project/Info.plist`, `docs/BUILDING.md` e os logs que eu enviar. Crie um plano de compilação **reproduzível em um Mac com Xcode**, preservando o iOS mínimo 17.0, ARM64 e Metal. Ajude-me a configurar as dependências, resolver erros sem apagar funcionalidades, gerar um IPA não assinado e, posteriormente, assiná-lo com **minha própria conta Apple**. Não invente resultados de compilação, não peça certificados ou senhas em texto e não tente contornar a assinatura do iOS. Não inclua ISO, BIOS nem arquivos protegidos do jogo no repositório. Preserve a licença GPL, todos os créditos do autor skuza-oss e os avisos de terceiros. Antes de modificar qualquer arquivo, indique a alteração, o motivo e o teste para validá-la. Considere AirPlay/HDMI, controles físicos, controles por toque e perfis 60/120 FPS recursos experimentais que precisam de testes em aparelho real.

### Como conduzir a conversa com a IA

1. Envie **uma mensagem de erro por vez**, com o comando executado e as últimas linhas do log.
2. Peça alterações pontuais, com indicação exata de arquivos, evitando reescrever a arquitetura sem necessidade.
3. Execute os comandos localmente no Mac e devolva os logs reais. **A IA não deve afirmar que testou Xcode se não executou Xcode.**
4. Rode testes em aparelho real: inicialização, importação da ISO, controles por toque/físicos, rotação, áudio, suspensão, AirPlay/HDMI, 60/120 FPS, aquecimento e resolução.
5. Registre cada alteração no Git e confira que o código-fonte correspondente ao IPA lançado estará disponível publicamente.

## 9. Solução de problemas frequentes

| Mensagem ou sintoma | O que conferir |
| :--- | :--- |
| `Missing build tools: xcodegen` | Instale XcodeGen e confirme seu `PATH` |
| `xcrun` não encontra SDK | Selecione o Xcode completo com `xcode-select` e aceite a licença |
| Falta `assets/vutext.bin` ou `iso/SCUS_971.77` | Prepare os dados do seu próprio disco e execute o fluxo de split |
| Erro de Rust para `aarch64-apple-ios` | Confira a configuração do target e da toolchain do repositório |
| `No provisioning profile` | Configure Team e Bundle ID no Xcode, ou use o sinalizador de atualização quando apropriado |
| IPA não instala | Se foi gerado com `--unsigned`, essa é a causa esperada; use uma assinatura Apple válida |
| O app abre, mas não carrega o jogo | Importe a ISO **USA SCUS-97177** não modificada no launcher |
| AirPlay ou 120 FPS oscilam | Verifique suporte de hardware, saída externa, energia e temperatura; faça testes reais |

## 10. Licenciamento na distribuição

Este fork continua sob GPL-3.0-or-later. Preserve o `LICENSE`, os avisos relevantes de `docs/CREDITS.md` e os avisos de terceiros. Disponibilize o código correspondente ao binário que publicar. O jogo comercial, suas marcas e seu conteúdo **não** estão licenciados pela GPL do projeto. Leia [`LEGAL_AND_CREDITS.md`](LEGAL_AND_CREDITS.md).
