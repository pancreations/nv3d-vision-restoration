cbuffer Params:register(b0){float4 screen;float4 state;float4 options;float4 source;float4 band;float4 extra;float4 leak0;float4 leak1;float4 capture;};
Texture2D image:register(t0);SamplerState imageSampler:register(s0);
struct V { float4 pos:SV_POSITION;float2 uv:TEXCOORD0; };
V vs(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.pos=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float3 linearize(float3 c){return lerp(c/12.92,pow(max((c+.055)/1.055,0),2.4),step(.04045,c));}
float3 srgb(float3 c){return lerp(12.92*c,1.055*pow(max(c,0),1/2.4)-.055,step(.0031308,c));}
float3 pq(float3 v){float3 p=pow(saturate(v),1/78.84375);return 125*pow(max(p-.8359375,0)/max(18.8515625-18.6875*p,1e-6),1/.1593017578125);}
// LightBoost crosstalk cancellation: the other eye's leak for this row of the stereo area, top to
// bottom, as eight samples from the illumination model (all zero when cancellation is off).
float leakAt(float y){float s[8]={leak0.x,leak0.y,leak0.z,leak0.w,leak1.x,leak1.y,leak1.z,leak1.w};float f=saturate(y)*7;int i=int(floor(f));int j=min(i+1,7);return lerp(s[i],s[j],f-i);}
float box(float2 p,float2 c,float2 r){float2 d=abs(p-c)-r;return 1-step(0,max(d.x,d.y));}
float glyph(float2 p,int letter){int2 xy=int2(floor(p));if(any(xy<0)||xy.x>=5||xy.y>=7)return 0;uint r[7];
if(letter==0){r[0]=16;r[1]=16;r[2]=16;r[3]=16;r[4]=16;r[5]=16;r[6]=31;}
else if(letter==1){r[0]=30;r[1]=17;r[2]=17;r[3]=30;r[4]=20;r[5]=18;r[6]=17;}
else if(letter==2){r[0]=31;r[1]=16;r[2]=16;r[3]=30;r[4]=16;r[5]=16;r[6]=31;}
else if(letter==3){r[0]=31;r[1]=16;r[2]=16;r[3]=30;r[4]=16;r[5]=16;r[6]=16;}
else if(letter==4){r[0]=31;r[1]=4;r[2]=4;r[3]=4;r[4]=4;r[5]=4;r[6]=4;}
else if(letter==5){r[0]=31;r[1]=4;r[2]=4;r[3]=4;r[4]=4;r[5]=4;r[6]=31;}
else if(letter==6){r[0]=15;r[1]=16;r[2]=16;r[3]=23;r[4]=17;r[5]=17;r[6]=15;}
else if(letter==7){r[0]=17;r[1]=17;r[2]=17;r[3]=31;r[4]=17;r[5]=17;r[6]=17;}
else if(letter==8){r[0]=30;r[1]=17;r[2]=17;r[3]=30;r[4]=16;r[5]=16;r[6]=16;}
else if(letter==9){r[0]=17;r[1]=17;r[2]=17;r[3]=17;r[4]=17;r[5]=10;r[6]=4;}
else if(letter==11){r[0]=15;r[1]=16;r[2]=16;r[3]=14;r[4]=1;r[5]=1;r[6]=30;}
else if(letter==12){r[0]=14;r[1]=17;r[2]=17;r[3]=17;r[4]=17;r[5]=17;r[6]=14;}
else if(letter==13){r[0]=17;r[1]=25;r[2]=21;r[3]=19;r[4]=17;r[5]=17;r[6]=17;}
else if(letter==14){r[0]=14;r[1]=17;r[2]=17;r[3]=31;r[4]=17;r[5]=17;r[6]=17;}
else if(letter==15){r[0]=14;r[1]=17;r[2]=16;r[3]=16;r[4]=16;r[5]=17;r[6]=14;}
else if(letter==20){r[0]=14;r[1]=17;r[2]=19;r[3]=21;r[4]=25;r[5]=17;r[6]=14;}
else if(letter==21){r[0]=4;r[1]=12;r[2]=4;r[3]=4;r[4]=4;r[5]=4;r[6]=14;}
else if(letter==22){r[0]=14;r[1]=17;r[2]=1;r[3]=2;r[4]=4;r[5]=8;r[6]=31;}
else if(letter==23){r[0]=31;r[1]=2;r[2]=4;r[3]=2;r[4]=1;r[5]=17;r[6]=14;}
else if(letter==24){r[0]=2;r[1]=6;r[2]=10;r[3]=18;r[4]=31;r[5]=2;r[6]=2;}
else if(letter==25){r[0]=31;r[1]=16;r[2]=30;r[3]=1;r[4]=1;r[5]=17;r[6]=14;}
else if(letter==26){r[0]=6;r[1]=8;r[2]=16;r[3]=30;r[4]=17;r[5]=17;r[6]=14;}
else if(letter==27){r[0]=31;r[1]=1;r[2]=2;r[3]=4;r[4]=8;r[5]=8;r[6]=8;}
else if(letter==28){r[0]=14;r[1]=17;r[2]=17;r[3]=14;r[4]=17;r[5]=17;r[6]=14;}
else if(letter==29){r[0]=14;r[1]=17;r[2]=17;r[3]=15;r[4]=1;r[5]=2;r[6]=12;}
else{r[0]=17;r[1]=17;r[2]=17;r[3]=21;r[4]=21;r[5]=21;r[6]=10;}
return (r[xy.y]>>(4-xy.x))&1;}
// Calibration readout: "PHASE nnnnn" drawn identically in both eyes (screen depth) so it fuses.
float phaseLabel(float2 uv,float value){int v=int(round(max(value,0)));int letters[11]={8,7,14,11,2,10,0,0,0,0,0};letters[5]=-1;
int digits[5];int q=v;for(int i=4;i>=0;i--){digits[i]=q%10;q/=10;}for(int i=0;i<5;i++)letters[6+i]=20+digits[i];
float2 p=(uv-float2(.5-11*.033,.9))/.011;float result=0;for(int i=0;i<11;i++)if(letters[i]>=0)result=max(result,glyph(p-float2(i*6,0),letters[i]));return result;}
float eyeLabel(float2 uv,int eye){int letters[5];letters[0]=eye==0?0:1;letters[1]=eye==0?2:5;letters[2]=eye==0?3:6;letters[3]=eye==0?4:7;letters[4]=4;float result=0;int len=eye==0?4:5;float2 p=(uv-float2(.5-len*.036,.08))/.012;for(int i=0;i<len;i++)result=max(result,glyph(p-float2(i*6,0),letters[i]));return result;}
float previewLabel(float2 uv){int letters[7]={8,1,2,9,5,2,10};float2 p=(uv-float2(.02,.93))/.006;float v=0;for(int i=0;i<7;i++)v=max(v,glyph(p-float2(i*6,0),letters[i]));return v;}
// Scene text is geometry, not a HUD: each word lies on a camera-facing plane at
// its own depth and is projected through the same stereo camera as the spheres,
// so both eyes see the same word with the disparity of that depth. Different
// LEFT/RIGHT words belong only in the identification patterns: they cannot fuse.
float labelHit(float3 ro,float3 rd,float3 origin,float cell,int word,int count){float t=(origin.z-ro.z)/rd.z;if(t<=0)return -1;float3 q=ro+rd*t;float2 g=float2((q.x-origin.x)/cell,(origin.y-q.y)/cell);
int words[18]={13,2,14,1,0,0, 11,15,1,2,2,13, 3,14,1,0,0,0};float ink=0;for(int i=0;i<count;i++)ink=max(ink,glyph(g-float2(i*6,0),words[word*6+i]));return ink>0?t:-1;}
float sphere(float3 ro,float3 rd,float3 c,float radius){float3 o=ro-c;float b=dot(o,rd),h=b*b-dot(o,o)+radius*radius;return h<0?-1:-b-sqrt(h);}
float3 scene(float2 uv,int eye,float aspect){float ipd=options.x;float3 ro=float3((eye==0?-.5:.5)*ipd,0,0);float2 p=(uv*2-1)*float2(aspect,-1);float3 rd=normalize(float3(p.x-ro.x*1.8/4,p.y,1.8));float best=100;float3 col=float3(.025,.035,.055);float3 centers[3]={float3(-.9,.1,3),float3(.1,-.1,4),float3(1,.25,6)};float3 colors[3]={float3(.1,.8,.65),float3(.85,.5,.12),float3(.25,.45,1)};
for(int i=0;i<3;i++){float3 c=centers[i];c.y+=sin(screen.z+i)*.15;float hit=sphere(ro,rd,c,.42);if(hit>0&&hit<best){best=hit;float3 n=normalize(ro+rd*hit-c);col=colors[i]*(.2+.8*saturate(dot(n,normalize(float3(-1,2,-2)))));}}
if(rd.y<-.01){float t=(-.7-ro.y)/rd.y;if(t>0&&t<best){float3 p3=ro+t*rd;float checker=fmod(abs(floor(p3.x*2)+floor(p3.z*2)),2);col=lerp(float3(.05,.07,.1),float3(.14,.17,.2),checker);}}
// NEAR floats in front of the screen plane (z=3), SCREEN lies on it (z=4, zero
// disparity) and FAR sits behind it (z=6.5). All three tops share screen height p.y=.85.
float3 origins[3]={float3(-1.467,1.4167,3),float3(-.6,1.889,4),float3(1.769,3.069,6.5)};float cells[3]={.04,.046,.075};int counts[3]={4,6,3};float3 inks[3]={float3(1,.6,.5),float3(.92,.92,.92),float3(.5,.7,1)};
for(int w=0;w<3;w++){float t=labelHit(ro,rd,origins[w],cells[w],w,counts[w]);if(t>0&&t<best){best=t;col=inks[w];}}
return col;}
float3 eyeColor(float2 uv,int eye,int pattern,float aspect,out bool black){black=false;float3 c=0;
// Move each eye in opposite directions without changing camera separation.
// Diagnostic targets stay fixed so convergence cannot hide optical leakage.
float2 stereoUV=uv;stereoUV.x+=(eye==0?1:-1)*options.z;
if(pattern==0){c=float3(.025,.03,.04);float mark=eye==0?box(uv,float2(.5,.52),float2(.15,.2)):1-step(.19,length((uv-float2(.5,.52))*float2(aspect,1)));c=lerp(c,float3(.8,.8,.8),mark);c=max(c,eyeLabel(uv,eye));}
else if(pattern==1){c=.005;for(int row=0;row<3;row++){float y=.25+row*.27;float level=row==1?.2:1;float x=eye==0?.32:.68;float target=box(uv,float2(x,y),float2(.065,.075));float cross=box(uv,float2(.5,y),float2(.025,.003))+box(uv,float2(.5,y),float2(.002,.033));c=max(c,target*level+cross*.08);}c=max(c,eyeLabel(uv,eye));}
else if(pattern==2){c=scene(stereoUV,eye,aspect);}
else if(pattern==3){if(stereoUV.x<0||stereoUV.x>1){black=true;return 0;}float2 p=stereoUV;if(source.x<.5)p.x=(p.x+eye)*.5;else p.y=(p.y+eye)*.5;float2 texel=1/max(source.zw,1);p=clamp(p,source.x<.5?float2(eye*.5,0)+texel*.5:float2(0,eye*.5)+texel*.5,source.x<.5?float2((eye+1)*.5,1)-texel*.5:float2(1,(eye+1)*.5)-texel*.5);c=image.Sample(imageSampler,p).rgb;if(source.y<.5)c=linearize(c);else if(source.y>1.5){c=pq(c);c=mul(float3x3(1.6605,-.5876,-.0728,-.1246,1.1329,-.0083,-.0182,-.1006,1.1187),c);}}
else if(pattern==5||pattern==6){if(eye!=pattern-5){black=true;return 0;}for(int row=0;row<3;row++)c=max(c,box(uv,float2(.5,.22+row*.28),float2(.22,.07)));c=max(c,eyeLabel(uv,eye));}
else if(pattern==7||(pattern>=9&&pattern<=11)){
    // Nine rows extending to the screen edges. Three transition pairs per row:
    // white/black, mid-gray/black, and light-gray/dark-gray, reversed in the other eye.
    int col=min(int(uv.x*6),5);float y=frac(uv.y*9);
    float levels[6]={1,0,.21404114,0,.60382734,.03310477};
    c=levels[eye==0?col:(col^1)];
    if(y<.035||y>.965)c=0;
    if(pattern>=9&&pattern<=11&&int(uv.y*3)==pattern-9&&(uv.x<.012||uv.x>.988))c=.5;
}
else{float x=floor(uv.x*8)/7;c=x;float patch=box(uv,float2(.5,.5),float2(.1,.15));c=lerp(c,options.y/80,patch);c=max(c,eyeLabel(uv,eye));}
return c;}
float4 ps(V input):SV_TARGET{
float2 uv=input.uv;int eye=int(state.x);bool preview=state.z>.5;float aspect=screen.x/screen.y;
if(preview){eye=uv.x<.5?0:1;uv.x=frac(uv.x*2);aspect*=.5;if(options.w>.5)eye=1-eye;}
if(int(state.y)==8&&(eye!=2||extra.z>.5)){
    // Actual submitted refresh index modulo 256, MSB on the left, repeated down
    // the screen so camera rolling-shutter mixing is visible. Diagnostic only.
    int cell=clamp(int((uv.x-.1)*10),0,7);uint code=uint(screen.w);
    float bit=float((code>>(7-cell))&1);float y=frac(uv.y*9);
    float3 c=(uv.x>=.1&&uv.x<.9&&y>.15&&y<.7)?bit:0;
    if(uv.x<.07&&y>.15&&y<.7)c=eye==0?float3(1,0,0):eye==1?float3(0,1,0):float3(0,0,1);
    if(y>.8)c=int(uv.x*16)==int(code%16)?1:0;
    return float4(state.w>.5?c*2.5:c,1);
}
if(eye==2){
    // Uniform neutral reset is explicit; startup/pause blanking sets extra.z to zero.
    float level=extra.z>.5?extra.w:0;
    float3 reset=state.w>.5?linearize(float3(level,level,level))*2.5:float3(level,level,level);
    return float4(reset,1);
}
// Stereo area: scale the image about the band center; everything outside stays black,
// so rows the panel is still rewriting carry nothing that could leak into the other eye.
if(band.x<.999){float2 local=(uv-float2(.5,band.y))/band.x+.5;if(any(local<0)||any(local>1))return float4(0,0,0,1);uv=local;}
int pattern=int(state.y);float3 c=0;
bool black=false;c=eyeColor(uv,eye,pattern,aspect,black);if(black)return float4(0,0,0,1);
// Crosstalk cancellation: the panel adds leak*other to what this eye sees, so subtracting the
// same share of the other eye's image (linear light) leaves the eye with its own image alone.
// Nothing can go below black, which is the one limit of the method.
{float k=leakAt(uv.y);if(k>0){bool otherBlack=false;float3 o=eyeColor(uv,1-eye,pattern,aspect,otherBlack);if(!otherBlack)c=max(c-k*o,0);}}
// Brightness compensation for a sequence that lights each eye on fewer refreshes. Applied before
// the readouts so they keep a fixed brightness, and never to a black frame.
c*=max(extra.x,1);
if(band.z>.5)c=lerp(c,float3(.85,.85,.85),phaseLabel(uv,band.w));
if(preview)c=lerp(c,float3(.1,.8,.7),previewLabel(input.uv));
// Native HDR/scRGB source values already use 80 nits per unit.
bool nativeHDR=pattern==3&&source.y>.5;
// LCD black floor (extra.y, fraction of the panel drive): the eye image never asks the liquid
// crystal for full black; the black frames still do (they returned 0 above), so a bright pixel is
// pulled down as hard as possible and only has to reach the floor before the shutter opens.
if(state.w>.5){if(!nativeHDR&&pattern!=4)c*=2.5;if(extra.y>0)c=max(c,pq(float3(extra.y,extra.y,extra.y)));return float4(c,1);}
if(nativeHDR)c=max(c,0)/max(capture.x,.01);float3 encoded=srgb(saturate(c));if(extra.y>0)encoded=extra.y+encoded*(1-extra.y);return float4(encoded,1);
}
