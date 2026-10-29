import glob
from PIL import Image
paths = sorted(p for p in glob.glob('snaps/*.ppm'))
ims = []
for p in paths:
    im = Image.open(p)
    im.save(p.replace('.ppm', '.png'))
    ims.append(im.resize((320, 240), Image.NEAREST) if im.size != (320, 240) else im)
if ims:
    W, H = ims[0].size; cols = 2; rows = (len(ims) + cols - 1) // cols
    sheet = Image.new('RGB', (W * cols + 10 * (cols - 1), H * rows + 10 * (rows - 1)), (40, 40, 40))
    for i, im in enumerate(ims): sheet.paste(im, ((i % cols) * (W + 10), (i // cols) * (H + 10)))
    sheet.save('snaps/sheet.png'); print(sheet.size)
