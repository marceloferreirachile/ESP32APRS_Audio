# Ajuste do áudio de TX (pré-ênfase) — guia opcional

**Você só precisa disto se o seu rádio transmissor entrega os dois tons do AFSK com forças diferentes.**
Os valores padrão (ganho 100%, pré-ênfase OFF, tilt 20%) funcionam para a maioria. Não mexa se o seu digi já é ouvido e decodificado bem.

## Quando vale a pena ajustar
O áudio do ESP32 entra no rádio por um pino de microfone. Muitos rádios aplicam pré-ênfase nesse pino, o que deixa o tom alto (2200 Hz) mais forte que o baixo (1200 Hz). Se a diferença for grande, algumas estações decodificam mal o seu sinal.
A página **Radio** tem três controles: **TX Audio Level** (ganho), **TX Pre-emphasis** (liga/desliga) e **Pre-emphasis Tilt** (quanto atenuar o tom de 1200 Hz, em %).

## Material
- Um segundo rádio receptor em 144,390 MHz (FM, squelch no mínimo), sempre no mesmo lugar e no mesmo volume.
- Um decibelímetro de celular (ou qualquer medidor de nível de áudio).

## Método
1. Na página Radio: ganho 100, pré-ênfase ON, tilt 0. Clique em **Apply Change** (aplica na hora).
2. Toque **Low Tone (1200 Hz)** e anote o nível médio (avg) e o máximo (max). Cada tom dura 10 s e desliga sozinho.
3. Toque **High Tone (2200 Hz)** e anote avg e max.
4. Compare. Se o High está mais forte que o Low, aumente o tilt; se o Low está mais forte, diminua.
5. O tilt atenua só o 1200 Hz. Como guia: 25% ≈ 2,5 dB, 50% ≈ 6 dB, 75% ≈ 12 dB.
6. Mude o tilt, **Apply Change**, repita os passos 2 e 3. Pare quando Low e High ficarem a 1–2 dB um do outro.
7. Anote o tilt final e salve a configuração (backup .cfg).

## Cuidados para a medida valer
- Não mexa no volume nem na posição do receptor entre um tom e outro.
- Compare só dentro da mesma sessão: o valor absoluto muda de um dia para o outro.
- Use sempre o mesmo ganho. O tom é transmissão real: faça com a antena adequada e por poucos segundos.
- Enquanto um tom toca, o escalonador de TX espera e os pacotes da fila saem logo depois. Se o digi estiver transmitindo quando você apertar, aparece um aviso: espere uns segundos e aperte de novo.

## Exemplo real (Motorola Radius M120, medido com um FT5D)
| Tilt | 1200 Hz | 2200 Hz | Resultado |
|------|---------|---------|-----------|
| 0 | ~5,5 dB acima | referência | 1200 Hz forte demais |
| 75 | ~7,5 dB abaixo | referência | 1200 Hz fraco demais |
| 45 | ≈ igual (diferença ~0,3 dB) | referência | equilibrado |

Esse valor (45) vale para esse rádio e essa ligação. O seu pode ser outro; por isso o método é o que importa.
