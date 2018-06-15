#version 150
uniform sampler2D image;
uniform sampler2D previous_image;
uniform bool mix_previous;

uniform vec2 output_resolution;
uniform vec2 origin;
const vec2 input_resolution = vec2(160, 144);

out vec4 frag_color;

vec4 modified_frag_cord;
#line 1
{filter}

void main()
{
    vec2 position = gl_FragCoord.xy - origin;
    position /= output_resolution;
    position.y = 1 - position.y;
    
    if (mix_previous) {
        frag_color = mix(scale(image, position), scale(previous_image, position), 0.5);
    }
    else {
        frag_color = scale(image, position);
    }
}
