
// The player's picture settings (present/picture.rs, finding 275), applied to the game's picture
// in the present pass: adjust is saturation, contrast, brightness and 1 / gamma, all zero while
// every setting is neutral, and then the colour is passed on as it is (today's exact bytes). The
// values are display-referred (the surfaces are UNORM, not sRGB): saturation mixes with the
// Rec. 709 luma, contrast scales around mid-grey, brightness scales, the result is held to 0..1,
// then gamma (above 1 lifts the mid-tones).
fn picture_adjust(colour: vec3<f32>, adjust: vec4<f32>) -> vec3<f32> {
    if (adjust.w == 0.0) {
        return colour;
    }
    let luma = dot(colour, vec3<f32>(0.2126, 0.7152, 0.0722));
    let saturated = mix(vec3<f32>(luma), colour, adjust.x);
    let contrasted = (saturated - 0.5) * adjust.y + 0.5;
    let bright = clamp(contrasted * adjust.z, vec3<f32>(0.0), vec3<f32>(1.0));
    return pow(bright, vec3<f32>(adjust.w));
}
