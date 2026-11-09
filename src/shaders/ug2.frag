varying vec2 vUV; varying vec3 vN; varying float vDepth; varying vec3 vPos;
varying vec4 vColor;
uniform sampler2D uTex;
uniform float uVColor;  /* 0 off, 1 world MODULATE2X, 2 car diffuse */
uniform float uUseTex; uniform vec3 uColor; uniform float uUnlit; uniform float uAlpha; uniform float uSoft; uniform float uSpec; uniform float uDecal;
uniform float uAmbient; uniform float uDiffuse; uniform vec3 uLight;
uniform vec3 uFogColor; uniform float uFogDensity;
uniform vec3 uCamPos; uniform float uEnv; uniform float uUVCheck;
uniform samplerCube uEnvCube; uniform float uEnvReady; uniform vec3 uEnvOrigin;
uniform float uGloss; uniform float uFlipN;
uniform float uWetness, uWeatherTime, uRainIntensity;
uniform vec4 uWetLightPos[8],uWetLightColor[8];
uniform float uPaintLights;
uniform float uRoadReflection; uniform vec4 uReflectionParams;
uniform sampler2D uReflectionColor,uReflectionDepth; uniform mat4 uMVP;
uniform float uRimTint;  /* >0: recolor the rim diffuse toward uColor */
uniform vec3 uEmissive;  /* lamp emission added on top of the lit result */
uniform float uVista;  /* >0.5: authored backdrop pass, alpha-blended; >1.5: tire-mark pass */
uniform float uAlphaTest;  /* >0.5: discard below 0.5 texture alpha */
uniform float uTextureAlpha;  /* >0.5: output alpha = texture2D(...).a *
   uAlpha instead of plain uAlpha (M135-R: N2_DRAW_BLEND/ADD world
   batches only -- everything else, including CUTOUT, cars, and every
   HUD/debug uUnlit pass, leaves this at its default 0.0 and keeps its
   existing uAlpha-only alpha output unchanged) */
uniform float uEmissiveTex;  /* authored texture-backed unlit pass */
uniform float uFresnel;  /* >0.5: glass -- fresnel drives alpha + reflection */
uniform float uClearcoat;  /* >0: second tight specular lobe over the base coat */
uniform vec4 uHeadPos[2];
uniform vec3 uHeadForward, uHeadRight, uHeadUp;
uniform vec4 uHeadShape;  /* horizontal/vertical spread, downward slope, range */
uniform float uHeadGain, uHeadLow;
uniform float uHeadShadow;
uniform sampler2D uHeadShadowMap0, uHeadShadowMap1;
uniform mat4 uHeadShadowMVP[2];
float headVisibility(int h,vec3 position,vec3 normal,float along){
  if(uHeadShadow<0.5)return 1.0;
  float offset=0.005+along*max(uHeadShape.x,uHeadShape.y)*2.0/512.0;
  vec4 clip=uHeadShadowMVP[h]*vec4(position+normal*offset,1.0);
  vec2 uv=clip.xy/clip.w*0.5+0.5;
  if(clip.w<=0.0 || uv.x<0.0 || uv.x>1.0 || uv.y<0.0 || uv.y>1.0)return 1.0;
  float depth=(clip.w-0.025-0.001*along)/uHeadShape.w;
  float visibility=0.0;
  for(int x=0;x<2;x++)for(int y=0;y<2;y++){
    vec2 at=uv+(vec2(float(x),float(y))-0.5)/512.0;
    vec2 depthRG=h==0?texture2D(uHeadShadowMap0,at).rg:texture2D(uHeadShadowMap1,at).rg;
    visibility+=step(depth,dot(depthRG,vec2(1.0,1.0/255.0)))*0.25;
  }
  return visibility;
}
float wetHash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float wetNoise(vec2 p){
  vec2 i=floor(p),f=fract(p); f=f*f*(3.0-2.0*f);
  return mix(mix(wetHash(i),wetHash(i+vec2(1,0)),f.x),mix(wetHash(i+vec2(0,1)),wetHash(i+vec2(1,1)),f.x),f.y);
}
/* Screen-space reflection: bounded trace against this frame's depth.
   Missing/offscreen geometry keeps the ordinary wet material underneath. */
vec4 roadReflection(vec3 N,vec3 V,float wet,float puddle){
  if(wet<0.001 || distance(vPos,uCamPos)>80.0)return vec4(0.0);
  vec3 direction=reflect(-V,N),origin=vPos+N*0.04;
  float previous=0.0;
  for(int i=0;i<32;i++){
    if(float(i)>=uReflectionParams.z)break;
    float stepSize=32.0/uReflectionParams.z;
    float t=0.3+pow((float(i)+1.0)*stepSize,2.0)*0.045;
    vec4 clip=uMVP*vec4(origin+direction*t,1.0);
    vec2 uv=clip.xy/clip.w*0.5+0.5;
    if(clip.w<=0.1 || min(uv.x,uv.y)<0.002 || max(uv.x,uv.y)>0.998)break;
    float depth=texture2D(uReflectionDepth,uv).r;
    float surface=uReflectionParams.y/(depth*2.0-1.0+uReflectionParams.x);
    if(depth<0.999999 && clip.w>surface){
      float lo=previous,hi=t;
      for(int j=0;j<5;j++){
        float mid=(lo+hi)*0.5;
        clip=uMVP*vec4(origin+direction*mid,1.0); uv=clip.xy/clip.w*0.5+0.5;
        depth=texture2D(uReflectionDepth,uv).r;
        surface=uReflectionParams.y/(depth*2.0-1.0+uReflectionParams.x);
        if(clip.w>surface)hi=mid;else lo=mid;
      }
      clip=uMVP*vec4(origin+direction*hi,1.0); uv=clip.xy/clip.w*0.5+0.5;
      depth=texture2D(uReflectionDepth,uv).r;
      surface=uReflectionParams.y/(depth*2.0-1.0+uReflectionParams.x);
      float gap=clip.w-surface;
      if(hi<0.6 || depth>=0.999999 || gap<0.0 || gap>0.4+surface*0.003)return vec4(0.0);
      float edge=smoothstep(0.0,0.08,min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y)));
      float fres=0.04+0.96*pow(1.0-max(dot(N,V),0.0),3.0);
      float fade=(1.0-smoothstep(30.0,46.0,hi))*(1.0-smoothstep(60.0,80.0,distance(vPos,uCamPos)));
      return vec4(texture2D(uReflectionColor,uv).rgb,edge*fade*fres*(0.18*wet+0.72*puddle));
    }
    previous=t;
  }
  return vec4(0.0);
}
/* exp^2 distance fog: fades far batches into the sky colour (which is
   cleared to uFogColor, so the horizon and the haze always agree) */
void main(){
/* diagnostic UV visualization: bypasses lighting/texture/fog entirely.
   R=u, G=v (raw, unclamped -- a mesh whose UVs run outside [0,1] shows
   as a flat saturated patch instead of a gradient, exposing tiling
   regions at a glance) with a darkened 10x10 grid overlaid so uneven
   cell spacing reveals stretching and grid-line direction reveals
   flips/mirrors. Checked first so it overrides every other path. */
  if(uUVCheck>0.5){
    vec2 g=abs(fract(vUV*10.0-0.5)-0.5);
    float grid=smoothstep(0.0,0.04,min(g.x,g.y));
    gl_FragColor=vec4(vec3(vUV.x,vUV.y,0.2)*mix(0.35,1.0,grid),1.0); return;
  }
  float fog = clamp(exp(-pow(vDepth*uFogDensity, 2.0)), 0.0, 1.0);
/* M132-R2 vista path. The backdrops are authored as cut-out sheets: their
   DXT1 blocks carry one-bit transparency (up to 57.7% of texels fully
   clear) and their DXT3 blocks carry real gradients, all of which the
   decoder used to throw away -- which is exactly why a panorama rendered
   as an opaque black-edged slab. Alpha here comes only from those two
   proven sources, texture alpha and source vertex-colour alpha; nothing is
   keyed off black pixels. Fully clear texels are discarded so they cannot
   write depth-adjacent artefacts or fringe. */
/* Tire-mark pass (tire-marks SPEC TM-15.1): texture x vertex colour, colour
   doubled, alpha NOT doubled, no lighting/tint/alpha test, world fog on the
   colour only. Vertex bytes are RGB 128 and alpha 0..127, so out.rgb is the
   texel colour x 256/255 and out.a = texel alpha x vertexAlpha/255. */
/* Original-style headlight (low detail): the pool texture is projected down onto
   the road ahead of the car, as the retail worldheadlight effect does, instead
   of the per-pixel beams below. uHeadPos[0] is the pool's near edge on the
   ground, uHeadForward/uHeadRight its axes, uHeadShape length, width and the
   height band it may touch. Additive, so it fades to black with fog. */
  if(uVista>2.5){
    vec3 d=vPos-uHeadPos[0].xyz;
    float f=dot(d,uHeadForward)/uHeadShape.x, s=dot(d,uHeadRight)/uHeadShape.y+0.5;
    if(f<0.0||f>1.0||s<0.0||s>1.0||abs(d.z)>uHeadShape.z)discard;
    float facing=smoothstep(0.35,0.8,normalize(vN).z);
    gl_FragColor=vec4(texture2D(uTex,vec2(f,s)).rgb*uColor*facing*fog,1.0);
    return;
  }
  if(uVista>1.5){
    vec4 t = texture2D(uTex, vUV);
    gl_FragColor = vec4(mix(uFogColor, clamp(2.0*t.rgb*vColor.rgb, 0.0, 1.0), fog), t.a*vColor.a);
    return;
  }
  if(uVista>0.5){
    vec4 t = texture2D(uTex, vUV);
    vec3 c = uUseTex>0.5 ? t.rgb : uColor;
    float a = (uUseTex>0.5 ? t.a : 1.0) * vColor.a;
    if(a < 0.02) discard;
    gl_FragColor = vec4(mix(uFogColor, c, fog), a);
    return;
  }
  if(uEmissiveTex>0.5){
    vec4 t=texture2D(uTex,vUV);
    float a=t.a*uAlpha; if(a<0.02) discard;
    gl_FragColor=vec4(mix(uFogColor,t.rgb*uColor,fog),a); return;
  }
  if(uUnlit>0.5){ float a=uAlpha;
/* Shadow footprints need a filled rounded rectangle; lamp/neon sprites
   retain their radial falloff. Both fade to zero at the quad boundary. */
    if(uSoft>0.5){
/* ponytail: transparent lens highlights, no refraction copy. Add image
   distortion only if its measured cost fits the frame budget. */
      if(uSoft>2.5){
        vec2 p=vUV*2.0-1.0; p.x*=1.0+0.12*p.y; float d=dot(p,p); if(d>1.0)discard;
        float rim=smoothstep(0.48,0.83,d)*(1.0-smoothstep(0.83,1.0,d));
        float glint=pow(max(0.0,1.0-length(p-vec2(-0.25,0.35))*3.0),3.0);
        float shade=clamp(0.35+p.y*0.65+glint,0.0,1.0);
        vec3 drop=mix(vec3(0.015,0.025,0.035),vec3(0.85,0.92,1.0),shade);
        gl_FragColor=vec4(drop,vColor.r*uAlpha*(0.04*(1.0-d)+0.26*rim+0.28*glint));return;
      }
      else if(uSoft>1.5){ float d=length(max(abs(vUV-vec2(0.5))-vec2(0.4),vec2(0.0))); a*=1.0-smoothstep(0.02,0.1,d); }
      else { float d=length(vUV-vec2(0.5)); a*=clamp(1.0-d*2.0,0.0,1.0); } a*=a; }
    gl_FragColor=vec4(mix(uFogColor,uColor,fog),a); return; }
/* Positions, normals, light and camera share world space. The model
   includes road tilt and each wheel/brake hub's own transform. */
  vec3 L=normalize(uLight); vec3 N=normalize(vN);
  if(uFlipN>0.5) N = -N;
  vec3 V=normalize(uCamPos - vPos);
/* Two-sided glass reflects the viewer-facing side; a backwards normal
   otherwise forces Fresnel to 1 and makes the far window fully opaque. */
  if(uFresnel>0.5 && dot(N,V)<0.0) N=-N;
/* Wet asphalt keeps a broad sheen; only sparse, nearly level patches
   collect standing water. Keep the reflection normal steady: unfiltered
   fine rain ripples alias at road distance and jump screen-space hits.
   ponytail: analytic sky/key-light fallback for offscreen geometry; use
   environment probes if reflections must survive outside the camera. */
  float wet=uWetness*smoothstep(0.55,0.9,N.z);
  float puddle=0.0;
  if(wet>0.001){
    float pools=0.7*wetNoise(vPos.xy*0.24)+0.3*wetNoise(vPos.xy*0.73);
    puddle=0.45*wet*smoothstep(0.66,0.85,pools)*smoothstep(0.94,0.99,N.z);
  }
  if(uRoadReflection>0.5){
    if(uAlphaTest>0.5 && texture2D(uTex,vUV).a<0.5)discard;
    vec4 reflected=roadReflection(N,V,wet,puddle); reflected.a*=fog;
    gl_FragColor=reflected;return;
  }
  float nl=max(dot(N,L),0.0);
  float d=uAmbient+uDiffuse*nl;  /* directional; reveals body form */
/* uAmbient/uDiffuse are one shared per-frame pair (world+cars both read
   them), so retuning the defaults to fix a car panel would also retune
   every building and road. Cars get their own lower ambient floor /
   sharper diffuse swing right here instead, gated the same way as the
   cavity term above -- world batches always set uSpec=0, so they never
   take this branch. */
/* Paint was clipping to white: at 0.55/1.2 the diffuse term alone reached
   ~0.95, which the shared *1.35 exposure below pushed past 1.0 before the
   specular, rim and environment terms were even added -- so every panel
   saturated and the highlights had no headroom left to show against.
   Pulling the body's own diffuse back leaves that headroom for them. */
  if(uSpec>0.001) d=uAmbient*0.35+uDiffuse*0.80*nl;
/* uDecal: paint under an alpha-masked decal atlas (badges/vinyls) --
   texture RGB shows only where its alpha says so, paint elsewhere */
  vec4 t = texture2D(uTex,vUV);
/* N2_DRAW_CUTOUT world batches and authored wheel textures. Threshold 0.5
   matches the authored railing/fence texture's own 1-bit DXT1 alpha
   (fully 0 or 255, no partial value to tune against). */
  if(uAlphaTest>0.5 && t.a<0.5) discard;
  vec3 base = uUseTex>0.5 ? (uDecal>0.5 ? mix(uColor,t.rgb,t.a) : t.rgb) : uColor;
/* Rim paint: recolor the diffuse toward uColor while KEEPING its detail.
   Luminance drives the shade, uColor picks the metal, so a gold OEM sheet
   becomes any painted colour (silver by default) with spokes/shadows
   intact. uRimTint 0 = raw OEM texture, 1 = fully painted. */
  if(uRimTint>0.001){ float y=dot(t.rgb, vec3(0.299,0.587,0.114));
    base = mix(base, uColor*(0.25+1.5*y), uRimTint); }
/* cheap view-angle cavity darkening (cars only, uSpec>0 -- the world
   batches always set uSpec=0): panel gaps/creases have no texture data
   to show them (verified: body meshes carry no diffuse map at all), so
   this fakes the early-2000s baked-AO look by darkening paint where the
   surface turns away from the camera, instead of claiming detail that
   isn't in the asset. Narrowed from pow4/0.6 to pow6/0.78: at pow4 the
   falloff reached far onto flat panels and fought the fresnel edge
   brightening below, which is what flattened the paint into matte clay.
   It now only bites in the last few degrees, i.e. actual creases. */
  if(uSpec>0.001){
    float edge=pow(1.0-clamp(dot(N,V),0.0,1.0), 6.0);
    base *= mix(1.0, 0.78, edge);
  }
/* Phong: reflect the light about the normal and test it against the VIEW
   vector. The old form used dot(N,L) with no V term at all, so it was a
   sharpened diffuse -- the highlight could not travel across a panel as
   the camera moved, which is what made the paint read flat/matte. */
  float rl = max(dot(reflect(-L,N), V), 0.0);
  float sp = pow(rl, uGloss)*uSpec;
/* clear coat: automotive paint is a coloured base coat under a clear
   lacquer, so it carries TWO highlights -- the broad soft one from the
   pigment (sp above) and a small hard one from the lacquer surface. One
   lobe alone is what makes painted metal read as moulded plastic. */
  sp += pow(rl, 160.0)*uClearcoat;
  float rim = pow(1.0-abs(N.z), 3.0)*uSpec*0.4;  /* fresnel-ish edge sheen */
/* The city capture supplies the clear-coat shape; the night key light
   must not clip the hood to white and hide those reflected buildings. */
  if(uEnvReady>0.5 && uClearcoat>0.001){sp*=0.30;rim*=0.25;}
  vec3 lit = base*d*1.35 + sp + rim;
/* World prelight uses MODULATE2X (0.5 neutral); textureless vehicle assets
   use the same byte slot as direct diffuse color for windows and trim. */
  if(uVColor>1.5) lit *= vColor.rgb;
  else if(uVColor>0.001) lit *= clamp(vColor.rgb*2.0, 0.0, 1.6);
  if(wet>0.001){
    lit*=1.0-0.32*wet;
    float wf=0.025+0.975*pow(1.0-max(dot(N,V),0.0),5.0);
    vec3 sky=mix(uFogColor,vec3(0.16,0.20,0.28),0.5);
    lit=mix(lit,sky,clamp(wf*(0.3*wet+0.65*puddle),0.0,0.85));
    lit+=vec3(0.65,0.72,0.85)*pow(rl,mix(36.0,120.0,puddle))*wet*0.35;
  }
/* ponytail: camera-local unshadowed district highlights. Add per-car light
   selection for distant cars, light shadows for leakage; headlights use shadows. */
  if(wet>0.001 || uPaintLights*uClearcoat>0.001)for(int i=0;i<8;i++){
    vec3 delta=uWetLightPos[i].xyz-vPos; float distance=length(delta);
    if(uWetLightColor[i].a>0.0 && distance<uWetLightPos[i].w){
      vec3 toLight=delta/max(distance,0.001);
      float reflected=max(dot(reflect(-toLight,N),V),0.0);
      float attenuation=1.0-smoothstep(0.0,uWetLightPos[i].w,distance);
      vec3 radiance=uWetLightColor[i].rgb*uWetLightColor[i].a*attenuation;
      if(wet>0.001 && delta.z>0.0)lit+=radiance*pow(reflected,mix(18.0,85.0,puddle))*wet*(0.3+1.7*puddle);
/* Existing clear-coat classification excludes rubber, cabin and plastic.
   No new scene pass or reflection copy: source lamps, normal and view
   determine a moving paint highlight in this same material draw. */
      if(uPaintLights*uClearcoat>0.001){
        float glint=0.3*uSpec*pow(reflected,max(uGloss,24.0))+0.65*uClearcoat*pow(reflected,96.0);
        lit+=radiance*glint*uPaintLights*max(dot(N,toLight),0.0);
      }
    }
  }
/* Surface lighting and per-lamp world-geometry shadows. */
  if(uHeadGain>0.0) for(int h=0;h<2;h++){
    vec3 ray=vPos-uHeadPos[h].xyz; float dist=length(ray);
    float along=dot(ray,uHeadForward);
    if(uHeadPos[h].w>0.5 && along>0.05 && dist<uHeadShape.w){
      float lateral=dot(ray,uHeadRight)/along;
      float elevation=dot(ray,uHeadUp)/along;
      vec2 cone=vec2(lateral/uHeadShape.x,(elevation+uHeadShape.z)/uHeadShape.y);
      float edge=1.0-smoothstep(0.55,1.0,length(cone));
      float cutoff=mix(1.0,1.0-smoothstep(-0.018,-0.008,elevation),uHeadLow);
      float reach=1.0-smoothstep(uHeadShape.w*0.65,uHeadShape.w,dist);
      float lambert=max(dot(N,-ray/max(dist,0.001)),0.0);
      if(edge*cutoff*lambert>0.0)lit+=base*vec3(1.0,0.94,0.82)*edge*cutoff*reach*lambert*uHeadGain*5.0*headVisibility(h,vPos,N,along)/(1.0+dist*dist/350.0);
      if(wet>0.001 && edge*cutoff>0.0){
        float highlight=pow(max(dot(reflect(ray/max(dist,0.001),N),V),0.0),mix(24.0,100.0,puddle));
        lit+=vec3(1.0,0.94,0.82)*highlight*wet*edge*cutoff*reach*uHeadGain*headVisibility(h,vPos,N,along)/(1.0+dist*dist/350.0);
      }
    }
  }
/* Local scene cube, sampled in world space so reflections stay anchored
   while the body banks. Fresnel adds clear-coat reflection without washing
   the base paint into the old constant warm horizon band. */
  float fres = 0.18 + 0.82*pow(1.0-clamp(dot(N,V),0.0,1.0), 3.0);
  if(uEnv>0.001){
    vec3 R = reflect(-V, N);
    float up = clamp(R.z*0.5+0.5, 0.0, 1.0);
    vec3 env = mix(vec3(0.025,0.025,0.035), vec3(0.10,0.14,0.24), up);
    float local=uEnvReady*(1.0-smoothstep(35.0,100.0,length(vPos-uEnvOrigin)));
    env=mix(env,textureCube(uEnvCube,R).rgb,local);
    lit=mix(lit,env,clamp(uEnv*fres,0.0,0.85));
  }
/* lit alpha = uAlpha (1 everywhere but the blended glass pass), so
   translucent glass keeps its specular highlight. M135-R: authored
   BLEND/ADD world batches and blended wheels multiply in the TEXTURE's own alpha
   (uTextureAlpha>0.5) -- otherwise a cutout-shaped blend/additive
   texture (e.g. a lit-window sheet with transparent gaps) would draw
   fully opaque/full-strength through those gaps, since uAlpha alone
   carries no per-texel information. */
/* Lamp emission. Added after every lighting term so a lit lens keeps its
   highlight, rim sheen and environment reflection and still reads as
   switched on. The dome weight is the same dot(N,V) the fresnel uses: a
   curved lens is hottest where its surface faces the camera and falls off
   toward the rim, which is the difference between a lens and a sticker.
   The 0.55 floor keeps the lamp clearly lit all over. */
  if(dot(uEmissive,uEmissive)>0.0) lit += uEmissive*(0.55+0.45*clamp(dot(N,V),0.0,1.0));
  float outA = uTextureAlpha>0.5 ? t.a*uAlpha : uAlpha;
/* Glass (uFresnel, the car glass pass only). A flat per-pass alpha is what
   made the cabin read as a still pool: every window sat at one constant
   opacity regardless of angle, so it looked like a filled surface rather
   than a pane. Real glass is nearly clear head-on and a mirror at grazing
   incidence, so alpha rides the same fresnel term the reflection does. */
  if(uFresnel>0.5) outA = clamp(mix(uAlpha, 1.0, fres), 0.0, 1.0);
  gl_FragColor=vec4(mix(uFogColor, lit, fog), outA);
}
