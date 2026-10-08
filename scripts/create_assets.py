from PIL import Image,ImageDraw
from pathlib import Path
size=256
img=Image.new('RGBA',(size,size),(0,0,0,0))
pix=img.load()
for y in range(size):
    for x in range(size):
        blend=(x+y)/(2*size)
        pix[x,y]=(int(141-58*blend),int(112-40*blend),int(242-49*blend),255)
mask=Image.new('L',(size,size),0)
ImageDraw.Draw(mask).rounded_rectangle((0,0,size-1,size-1),radius=64,fill=255)
img.putalpha(mask)
draw=ImageDraw.Draw(img)
draw.ellipse((62,58,188,184),fill=(249,248,255,255))
draw.ellipse((107,39,213,145),fill=(108,86,220,255))
draw.polygon([(177,163),(182,174),(194,178),(182,183),(177,195),(172,183),(160,178),(172,174)],fill=(132,242,210,255))
draw.ellipse((193,79,202,88),fill=(191,179,255,255))
img.save(Path(__file__).resolve().parents[1]/'resources/app.ico',sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
print('Application icon generated.')
