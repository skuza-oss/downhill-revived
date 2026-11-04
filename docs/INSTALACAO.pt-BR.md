# Como instalar o Downhill Revived no iPhone ou iPad

[Voltar ao README](../README.pt-BR.md) | [English](INSTALLATION.md)

## Compatibilidade

- **iOS ou iPadOS 17.0 ou superior**.
- Aparelho com arquitetura **ARM64** e suporte a **Metal**.
- **ISO original de Downhill Domination USA (SCUS-97177)**, criada a partir de seu próprio disco, sem modificações.
- Espaço de armazenamento suficiente para o aplicativo e a ISO do jogo.

## Atenção: o IPA disponível não está assinado

O arquivo `DownhillRevived-iOS-0.7.9-build-70902-unsigned.ipa` é um pacote de desenvolvimento **sem assinatura e provisionamento Apple válidos**. O simples download ou a ativação do Modo de Desenvolvedor **não permite instalá-lo diretamente** em um iPhone comum.

Para instalar, você deve gerar ou obter uma compilação assinada adequadamente, utilizando os métodos autorizados de desenvolvimento/distribuição Apple. Veja o [tutorial para compilar o IPA com IA](BUILD_IPA_WITH_AI.pt-BR.md).

## Passo a passo para quem tem Mac e Xcode

1. Configure sua conta Apple e sua equipe de assinatura no Xcode.
2. Compile o app com o Team ID próprio, identificador de pacote e perfil de provisionamento válidos para o dispositivo.
3. Conecte, emparelhe e autorize o iPhone/iPad no Mac. Ative o **Modo de Desenvolvedor** se o sistema exigir para a instalação de desenvolvimento.
4. Instale a versão assinada pelo Xcode ou usando o script do projeto com `--install`.
5. Abra o launcher e importe **sua ISO americana SCUS-97177** na área de gerenciamento de disco.
6. Ajuste os controles e os gráficos, entre no jogo e, se desejar, teste a saída HDMI ou AirPlay compatível.

Assinaturas de desenvolvimento podem ter restrições quanto ao dispositivo, à conta e à validade. Um IPA assinado para desenvolvimento **não é automaticamente um IPA distribuível para qualquer pessoa**.

## Qual ISO precisa ter no celular?

É a imagem de disco de **Downhill Domination para PlayStation 2, versão USA, código SCUS-97177**, no formato `.iso`. A versão europeia ou japonesa e ISOs modificadas não são suportadas pelo projeto original. O jogo não vem embutido no aplicativo.

## Qual versão do iOS precisa ter?

**iOS 17.0 ou superior.** A exigência está registrada dentro do próprio IPA analisado (`MinimumOSVersion = 17.0`). Também é necessário hardware ARM64/Metal. Ter iOS 17 por si só não dispensa assinatura do app nem a ISO do jogo.

## Como conferir o download

SHA-256 do **IPA não assinado enviado para a primeira publicação**:

```text
4181572bd7b78ce87e956d52309e0879b3230f82a7511d325a6abd2b50adcc4c
```

Após assinar ou recompilar, o checksum será diferente. Use os valores fornecidos com a versão específica do arquivo.

## Problemas comuns

- **Não instala:** confira assinatura, certificado, perfil Apple, Bundle ID e elegibilidade do dispositivo.
- **Abre, mas não roda o jogo:** importe a ISO original da edição **USA SCUS-97177**.
- **AirPlay/HDMI não funciona:** o receptor, o cabo/adaptador e os modos de vídeo disponíveis podem limitar a funcionalidade; teste em hardware real.
- **Não atinge 120 FPS:** a taxa depende da tela de 120 Hz, da temperatura, da energia e do desempenho do aparelho.

Leia também [créditos e questões jurídicas](LEGAL_AND_CREDITS.md).
